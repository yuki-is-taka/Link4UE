// Copyright YUKITAKA. All Rights Reserved.

#include "Link4UESubsystem.h"
#include "Link4UESettings.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "ISubmixBufferListener.h"
#include "AudioMixerDevice.h"
#include "AudioMixerSubmix.h"
#include "ableton/LinkAudio.hpp"
#include "ableton/util/FloatIntConversion.hpp"
#include "DSP/MultithreadedPatching.h"

namespace
{
	FString NodeIdToHex(const ableton::link::NodeId& Id)
	{
		FString Out;
		Out.Reserve(16);
		for (uint8 Byte : Id)
		{
			Out += FString::Printf(TEXT("%02x"), Byte);
		}
		return Out;
	}

	FAudioDevice* GetMainAudioDevice()
	{
		if (FAudioDeviceManager* ADM = FAudioDeviceManager::Get())
		{
			return ADM->GetActiveAudioDevice().GetAudioDevice();
		}
		return nullptr;
	}
}

DEFINE_LOG_CATEGORY_STATIC(LogLink4UE, Log, All);

// ---------------------------------------------------------------------------
// FLink4UESendBridge — captures Submix audio and commits to a LinkAudio Sink
// ---------------------------------------------------------------------------

class FLink4UESendBridge : public ISubmixBufferListener
{
public:
	FLink4UESendBridge(ableton::LinkAudio& InLink, const FString& ChannelName,
		size_t MaxNumSamples, const std::atomic<double>& InQuantumRef)
		: Sink(InLink, TCHAR_TO_UTF8(*ChannelName), MaxNumSamples)
		, LinkRef(InLink)
		, QuantumRef(InQuantumRef)
		, ListenerName(FString::Printf(TEXT("Link4UE_Send_%s"), *ChannelName))
	{
	}

	// ISubmixBufferListener
	virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData,
		int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override
	{
		if (NumChannels <= 0 || NumSamples <= 0)
		{
			return;
		}

		const int32 NumFrames = NumSamples / NumChannels;

		// LinkAudio Sink supports 1 or 2 channels only.
		// For 3+ channel Submixes, take the first 2 channels (stereo downmix fallback).
		const int32 LinkChannels = FMath::Min(NumChannels, 2);
		const int32 TotalLinkSamples = NumFrames * LinkChannels;

		// Ensure Sink buffer is large enough
		if (static_cast<size_t>(TotalLinkSamples) > Sink.maxNumSamples())
		{
			Sink.requestMaxNumSamples(static_cast<size_t>(TotalLinkSamples));
		}

		ableton::LinkAudioSink::BufferHandle Buffer(Sink);
		if (!Buffer)
		{
			return;
		}

		// Convert float → int16, handling channel reduction if needed
		if (NumChannels <= 2)
		{
			// Mono or stereo — direct interleaved copy
			for (int32 i = 0; i < TotalLinkSamples; ++i)
			{
				Buffer.samples[i] = ableton::util::floatToInt16(AudioData[i]);
			}
		}
		else
		{
			// 3+ channels — extract first 2 channels (interleaved)
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				const int32 SrcBase = Frame * NumChannels;
				const int32 DstBase = Frame * LinkChannels;
				for (int32 Ch = 0; Ch < LinkChannels; ++Ch)
				{
					Buffer.samples[DstBase + Ch] =
						ableton::util::floatToInt16(AudioData[SrcBase + Ch]);
				}
			}
		}

		// Capture session state (RT-safe) and commit
		const double Q = QuantumRef.load(std::memory_order_relaxed);
		const auto Now = LinkRef.clock().micros();
		const auto State = LinkRef.captureAudioSessionState();
		const double BeatsAtBegin = State.beatAtTime(Now, Q);

		Buffer.commit(State, BeatsAtBegin, Q,
			static_cast<size_t>(NumFrames),
			static_cast<size_t>(LinkChannels),
			static_cast<uint32_t>(SampleRate));
	}

	virtual const FString& GetListenerName() const override
	{
		return ListenerName;
	}

private:
	ableton::LinkAudioSink Sink;
	ableton::LinkAudio& LinkRef;
	const std::atomic<double>& QuantumRef;
	FString ListenerName;
};

// ---------------------------------------------------------------------------
// FLink4UEReceiveBridge — receives LinkAudio Source data and pushes to Submix
// ---------------------------------------------------------------------------

class FLink4UEReceiveBridge
{
public:
	FLink4UEReceiveBridge(ableton::LinkAudio& InLink, const ableton::ChannelId& InChannelId,
		Audio::FPatchInput&& InPatchInput, int32 InSubmixChannelIndex, int32 InSubmixNumChannels)
		: PatchInput(MoveTemp(InPatchInput))
		, SubmixChannelIndex(InSubmixChannelIndex)
		, SubmixNumChannels(InSubmixNumChannels)
		, Source(InLink, InChannelId,
			[this](ableton::LinkAudioSource::BufferHandle BufferHandle)
			{
				OnSourceBuffer(BufferHandle);
			})
	{
	}

	~FLink4UEReceiveBridge()
	{
		// Source destructor unsubscribes the callback
	}

	uint32 GetUnderrunCount() const { return UnderrunCount.load(std::memory_order_relaxed); }
	uint32 GetOverrunCount() const { return OverrunCount.load(std::memory_order_relaxed); }

private:
	void OnSourceBuffer(const ableton::LinkAudioSource::BufferHandle BufferHandle)
	{
		const size_t NumFrames = BufferHandle.info.numFrames;
		const size_t SrcChannels = BufferHandle.info.numChannels;

		if (NumFrames == 0)
		{
			return;
		}

		// Convert int16 → float and map to target submix channels
		const int32 OutSamples = static_cast<int32>(NumFrames) * SubmixNumChannels;

		// Resize conversion buffer if needed (only grows)
		if (OutSamples > ConversionBuffer.Num())
		{
			ConversionBuffer.SetNumZeroed(OutSamples);
		}
		else
		{
			FMemory::Memzero(ConversionBuffer.GetData(), OutSamples * sizeof(float));
		}

		for (int32 Frame = 0; Frame < static_cast<int32>(NumFrames); ++Frame)
		{
			if (SubmixChannelIndex < 0)
			{
				// Replicate to all channels
				const float Sample = ableton::util::int16ToFloat<float>(
					BufferHandle.samples[Frame * SrcChannels]);
				for (int32 Ch = 0; Ch < SubmixNumChannels; ++Ch)
				{
					ConversionBuffer[Frame * SubmixNumChannels + Ch] = Sample;
				}
			}
			else if (SubmixChannelIndex < SubmixNumChannels)
			{
				// Place at specific channel index
				const float Sample = ableton::util::int16ToFloat<float>(
					BufferHandle.samples[Frame * SrcChannels]);
				ConversionBuffer[Frame * SubmixNumChannels + SubmixChannelIndex] = Sample;
			}
		}

		const int32 Pushed = PatchInput.PushAudio(ConversionBuffer.GetData(), OutSamples);
		if (Pushed < OutSamples)
		{
			OverrunCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	Audio::FPatchInput PatchInput;
	int32 SubmixChannelIndex;
	int32 SubmixNumChannels;
	ableton::LinkAudioSource Source;
	TArray<float> ConversionBuffer;
	std::atomic<uint32> UnderrunCount{0};
	std::atomic<uint32> OverrunCount{0};
};

// ---------------------------------------------------------------------------
// Pimpl — hides ableton::LinkAudio from the header
// ---------------------------------------------------------------------------

struct ULink4UESubsystem::FLinkInstance
{
	ableton::LinkAudio Link;

	// Active send bridges (Submix → LinkAudio Sink)
	struct FActiveSend
	{
		TSharedPtr<FLink4UESendBridge, ESPMode::ThreadSafe> Bridge;
		TWeakObjectPtr<USoundSubmix> Submix;
	};
	TArray<FActiveSend> ActiveSends;

	// Active receive bridges (LinkAudio Source → Submix)
	TArray<TUniquePtr<FLink4UEReceiveBridge>> ActiveReceives;

	FLinkInstance(double BPM, const std::string& PeerName)
		: Link(BPM, PeerName)
	{
	}

	void TearDownSends()
	{
		FAudioDevice* AudioDevice = GetMainAudioDevice();
		for (auto& Send : ActiveSends)
		{
			if (AudioDevice && Send.Bridge.IsValid() && Send.Submix.IsValid())
			{
				AudioDevice->UnregisterSubmixBufferListener(
					Send.Bridge.ToSharedRef(), *Send.Submix.Get());
			}
		}
		ActiveSends.Empty();
	}

	void TearDownReceives()
	{
		// Source destructors unsubscribe callbacks; PatchInput destructors disconnect patches
		ActiveReceives.Empty();
	}
};

// ---------------------------------------------------------------------------
// Constructor / Destructor — defined here so FLinkInstance is complete
// ---------------------------------------------------------------------------

ULink4UESubsystem::ULink4UESubsystem() = default;

ULink4UESubsystem::~ULink4UESubsystem()
{
	delete LinkInstance;
	LinkInstance = nullptr;
}

// ---------------------------------------------------------------------------
// USubsystem interface
// ---------------------------------------------------------------------------

void ULink4UESubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Read settings
	const ULink4UESettings* Settings = GetDefault<ULink4UESettings>();
	const double QuantumBeats = Link4UEQuantumToBeats(Settings->DefaultQuantum);
	Snapshot.Tempo = Settings->DefaultTempo;
	Snapshot.Quantum = QuantumBeats;
	Quantum.store(QuantumBeats, std::memory_order_relaxed);

	LinkInstance = new FLinkInstance(Settings->DefaultTempo,
		TCHAR_TO_UTF8(*Settings->PeerName));

	// Register Link-thread callbacks — store values atomically, consume on GameThread
	LinkInstance->Link.setNumPeersCallback([this](std::size_t InNumPeers)
	{
		PendingNumPeers.store(static_cast<int32>(InNumPeers), std::memory_order_relaxed);
		bNumPeersDirty.store(true, std::memory_order_release);
	});

	LinkInstance->Link.setTempoCallback([this](double InBPM)
	{
		PendingTempo.store(InBPM, std::memory_order_relaxed);
		bTempoDirty.store(true, std::memory_order_release);
	});

	LinkInstance->Link.setStartStopCallback([this](bool bInIsPlaying)
	{
		PendingIsPlaying.store(bInIsPlaying, std::memory_order_relaxed);
		bStartStopDirty.store(true, std::memory_order_release);
	});

	// Tick on GameThread via FTSTicker
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ULink4UESubsystem::Tick));

	// LinkAudio channels-changed callback
	LinkInstance->Link.setChannelsChangedCallback([this]()
	{
		bChannelsDirty.store(true, std::memory_order_release);
	});

	// Apply settings
	ApplySettings(Settings);

#if WITH_EDITOR
	SettingsChangedHandle = ULink4UESettings::OnSettingsChanged.AddUObject(
		this, &ULink4UESubsystem::OnSettingsChanged);
#endif
}

void ULink4UESubsystem::Deinitialize()
{
#if WITH_EDITOR
	ULink4UESettings::OnSettingsChanged.Remove(SettingsChangedHandle);
#endif
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);

	if (LinkInstance)
	{
		LinkInstance->TearDownReceives();
		LinkInstance->TearDownSends();
		LinkInstance->Link.enableLinkAudio(false);
		LinkInstance->Link.enable(false);
		delete LinkInstance;
		LinkInstance = nullptr;
	}

	UE_LOG(LogLink4UE, Log, TEXT("Link4UE subsystem deinitialized"));
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Settings hot-reload
// ---------------------------------------------------------------------------

void ULink4UESubsystem::ApplySettings(const ULink4UESettings* Settings)
{
	if (!LinkInstance || !Settings)
	{
		return;
	}

	LinkInstance->Link.enableStartStopSync(Settings->bStartStopSync);
	LinkInstance->Link.enableLinkAudio(Settings->bEnableLinkAudio);
	LinkInstance->Link.setPeerName(TCHAR_TO_UTF8(*Settings->PeerName));
	LinkInstance->Link.enable(Settings->bAutoConnect);

	const double Q = Link4UEQuantumToBeats(Settings->DefaultQuantum);
	Quantum.store(Q, std::memory_order_relaxed);

	SetTempo(Settings->DefaultTempo);

	// --- Rebuild send routes ---
	LinkInstance->TearDownSends();

	if (Settings->bEnableLinkAudio)
	{
		FAudioDevice* AudioDevice = GetMainAudioDevice();
		if (AudioDevice)
		{
			for (const FLink4UEAudioSend& SendDef : Settings->AudioSends)
			{
				USoundSubmix* Submix = SendDef.Submix.LoadSynchronous();
				if (!Submix)
				{
					UE_LOG(LogLink4UE, Warning,
						TEXT("Link4UE: AudioSend skipped — Submix asset not found"));
					continue;
				}

				// Determine channel name: explicit prefix or Submix asset name
				FString ChannelName = SendDef.ChannelNamePrefix.IsEmpty()
					? Submix->GetName()
					: SendDef.ChannelNamePrefix;

				constexpr size_t kDefaultMaxSamples = 8192;
				TSharedRef<FLink4UESendBridge, ESPMode::ThreadSafe> Bridge =
					MakeShared<FLink4UESendBridge, ESPMode::ThreadSafe>(
						LinkInstance->Link, ChannelName, kDefaultMaxSamples, Quantum);

				AudioDevice->RegisterSubmixBufferListener(Bridge, *Submix);

				FLinkInstance::FActiveSend& ActiveSend = LinkInstance->ActiveSends.AddDefaulted_GetRef();
				ActiveSend.Bridge = Bridge;
				ActiveSend.Submix = Submix;

				UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Send route created [%s] → Sink '%s'"),
					*Submix->GetName(), *ChannelName);
			}
		}
		else
		{
			UE_LOG(LogLink4UE, Warning,
				TEXT("Link4UE: AudioDevice not available — send routes deferred"));
		}
	}

	// --- Rebuild receive routes ---
	LinkInstance->TearDownReceives();

	if (Settings->bEnableLinkAudio)
	{
		Audio::FMixerDevice* MixerDevice = nullptr;
		{
			FAudioDevice* AudioDevice = GetMainAudioDevice();
			if (AudioDevice)
			{
				MixerDevice = static_cast<Audio::FMixerDevice*>(AudioDevice);
			}
		}

		if (MixerDevice)
		{
			// Resolve channel names to ChannelIds from current session
			const auto AllChannels = LinkInstance->Link.channels();

			for (const FLink4UEAudioReceive& RecvDef : Settings->AudioReceives)
			{
				USoundSubmix* Submix = RecvDef.Submix.LoadSynchronous();
				if (!Submix)
				{
					UE_LOG(LogLink4UE, Warning,
						TEXT("Link4UE: AudioReceive skipped — Submix asset not found"));
					continue;
				}

				// Find matching channel by name
				const ableton::ChannelId* FoundId = nullptr;
				for (const auto& Ch : AllChannels)
				{
					if (FString(UTF8_TO_TCHAR(Ch.name.c_str())) == RecvDef.ChannelName)
					{
						FoundId = &Ch.id;
						break;
					}
				}

				if (!FoundId)
				{
					UE_LOG(LogLink4UE, Warning,
						TEXT("Link4UE: AudioReceive skipped — channel '%s' not found in session"),
						*RecvDef.ChannelName);
					continue;
				}

				// Get the mixer submix to add a patch
				Audio::FMixerSubmixWeakPtr SubmixWeakPtr = MixerDevice->GetSubmixInstance(Submix);
				Audio::FMixerSubmixPtr SubmixPtr = SubmixWeakPtr.Pin();
				if (!SubmixPtr.IsValid())
				{
					UE_LOG(LogLink4UE, Warning,
						TEXT("Link4UE: AudioReceive skipped — mixer submix not found for '%s'"),
						*Submix->GetName());
					continue;
				}

				// Determine submix channel count (default to device output channels)
				const int32 SubmixNumChannels = FMath::Max(
					MixerDevice->GetNumDeviceChannels(), 2);

				// AddPatch creates the PatchOutput inside the submix and returns it
				Audio::FPatchOutputStrongPtr PatchOutput = SubmixPtr->AddPatch(1.0f);
				Audio::FPatchInput PatchInput(PatchOutput);

				// Create the receive bridge
				LinkInstance->ActiveReceives.Add(MakeUnique<FLink4UEReceiveBridge>(
					LinkInstance->Link, *FoundId,
					MoveTemp(PatchInput), RecvDef.SubmixChannelIndex, SubmixNumChannels));

				UE_LOG(LogLink4UE, Log,
					TEXT("Link4UE: Receive route created '%s' → Submix '%s' (ch=%d)"),
					*RecvDef.ChannelName, *Submix->GetName(), RecvDef.SubmixChannelIndex);
			}
		}
		else
		{
			UE_LOG(LogLink4UE, Warning,
				TEXT("Link4UE: MixerDevice not available — receive routes deferred"));
		}
	}

	UE_LOG(LogLink4UE, Log, TEXT("Link4UE settings applied (tempo=%.1f, auto=%s, audio=%s, peer=%s)"),
		Settings->DefaultTempo,
		Settings->bAutoConnect ? TEXT("on") : TEXT("off"),
		Settings->bEnableLinkAudio ? TEXT("on") : TEXT("off"),
		*Settings->PeerName);
}

#if WITH_EDITOR
void ULink4UESubsystem::OnSettingsChanged()
{
	ApplySettings(GetDefault<ULink4UESettings>());
}
#endif

// ---------------------------------------------------------------------------
// Tick — capture session state & dispatch delegates
// ---------------------------------------------------------------------------

bool ULink4UESubsystem::Tick(float DeltaTime)
{
	if (!LinkInstance)
	{
		return true; // keep ticking
	}

	const double Q = Quantum.load(std::memory_order_relaxed);
	const auto Now = LinkInstance->Link.clock().micros();
	const auto State = LinkInstance->Link.captureAppSessionState();

	Snapshot.Tempo = State.tempo();
	Snapshot.Beat = State.beatAtTime(Now, Q);
	Snapshot.Phase = State.phaseAtTime(Now, Q);
	Snapshot.bIsPlaying = State.isPlaying();
	Snapshot.NumPeers = static_cast<int32>(LinkInstance->Link.numPeers());
	Snapshot.Quantum = Q;

	// Beat / Phase-zero edge detection
	const int32 CurBeatFloor = FMath::FloorToInt32(Snapshot.Beat);
	if (PrevBeatFloor >= 0 && CurBeatFloor > PrevBeatFloor)
	{
		// Fire for each beat crossed (handles multiple beats per frame)
		for (int32 B = PrevBeatFloor + 1; B <= CurBeatFloor; ++B)
		{
			const bool bPhaseZero = FMath::IsNearlyZero(FMath::Fmod(static_cast<double>(B), Q), 0.001);
			OnBeat.Broadcast(B, bPhaseZero);
			if (bPhaseZero)
			{
				OnPhaseZero.Broadcast();
			}
		}
	}
	PrevBeatFloor = CurBeatFloor;

	// Dispatch callbacks accumulated from Link thread
	if (bNumPeersDirty.exchange(false, std::memory_order_acquire))
	{
		OnNumPeersChanged.Broadcast(PendingNumPeers.load(std::memory_order_relaxed));
	}
	if (bTempoDirty.exchange(false, std::memory_order_acquire))
	{
		OnTempoChanged.Broadcast(PendingTempo.load(std::memory_order_relaxed));
	}
	if (bStartStopDirty.exchange(false, std::memory_order_acquire))
	{
		OnStartStopChanged.Broadcast(PendingIsPlaying.load(std::memory_order_relaxed));
	}
	if (bChannelsDirty.exchange(false, std::memory_order_acquire))
	{
		OnChannelsChanged.Broadcast();
	}

	return true; // keep ticking
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

double ULink4UESubsystem::GetTimeAtBeat(double Beat) const
{
	if (!LinkInstance) return 0.0;

	const auto State = LinkInstance->Link.captureAppSessionState();
	const double Q = Quantum.load(std::memory_order_relaxed);
	const auto Micros = State.timeAtBeat(Beat, Q);
	return std::chrono::duration<double>(Micros).count();
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------

void ULink4UESubsystem::SetTempo(double BPM)
{
	if (!LinkInstance) return;

	auto State = LinkInstance->Link.captureAppSessionState();
	State.setTempo(BPM, LinkInstance->Link.clock().micros());
	LinkInstance->Link.commitAppSessionState(State);
}

void ULink4UESubsystem::SetQuantum(double InQuantum)
{
	Quantum.store(InQuantum, std::memory_order_relaxed);
}

void ULink4UESubsystem::SetQuantumPreset(ELink4UEQuantum Preset)
{
	Quantum.store(Link4UEQuantumToBeats(Preset), std::memory_order_relaxed);
}

void ULink4UESubsystem::SetIsPlaying(bool bPlay)
{
	if (!LinkInstance) return;

	auto State = LinkInstance->Link.captureAppSessionState();
	const auto Now = LinkInstance->Link.clock().micros();
	const double Q = Quantum.load(std::memory_order_relaxed);

	if (bPlay)
	{
		State.setIsPlayingAndRequestBeatAtTime(true, Now, 0.0, Q);
	}
	else
	{
		State.setIsPlaying(false, Now);
	}
	LinkInstance->Link.commitAppSessionState(State);
}

void ULink4UESubsystem::RequestBeatAtTime(double Beat)
{
	if (!LinkInstance) return;

	auto State = LinkInstance->Link.captureAppSessionState();
	const auto Now = LinkInstance->Link.clock().micros();
	const double Q = Quantum.load(std::memory_order_relaxed);

	State.requestBeatAtTime(Beat, Now, Q);
	LinkInstance->Link.commitAppSessionState(State);
}

// ---------------------------------------------------------------------------
// Enable / Disable
// ---------------------------------------------------------------------------

void ULink4UESubsystem::EnableLink()
{
	if (LinkInstance)
	{
		LinkInstance->Link.enable(true);
		UE_LOG(LogLink4UE, Log, TEXT("Link enabled"));
	}
}

void ULink4UESubsystem::DisableLink()
{
	if (LinkInstance)
	{
		LinkInstance->Link.enable(false);
		UE_LOG(LogLink4UE, Log, TEXT("Link disabled"));
	}
}

bool ULink4UESubsystem::IsLinkEnabled() const
{
	return LinkInstance && LinkInstance->Link.isEnabled();
}

void ULink4UESubsystem::EnableStartStopSync(bool bEnable)
{
	if (LinkInstance)
	{
		LinkInstance->Link.enableStartStopSync(bEnable);
	}
}

bool ULink4UESubsystem::IsStartStopSyncEnabled() const
{
	return LinkInstance && LinkInstance->Link.isStartStopSyncEnabled();
}

// ---------------------------------------------------------------------------
// LinkAudio
// ---------------------------------------------------------------------------

void ULink4UESubsystem::EnableLinkAudio(bool bEnable)
{
	if (LinkInstance)
	{
		LinkInstance->Link.enableLinkAudio(bEnable);
		UE_LOG(LogLink4UE, Log, TEXT("Link Audio %s"), bEnable ? TEXT("enabled") : TEXT("disabled"));
	}
}

bool ULink4UESubsystem::IsLinkAudioEnabled() const
{
	return LinkInstance && LinkInstance->Link.isLinkAudioEnabled();
}

void ULink4UESubsystem::SetPeerName(const FString& InPeerName)
{
	if (LinkInstance)
	{
		LinkInstance->Link.setPeerName(TCHAR_TO_UTF8(*InPeerName));
	}
}

TArray<FLink4UEChannel> ULink4UESubsystem::GetChannels() const
{
	TArray<FLink4UEChannel> Result;
	if (!LinkInstance)
	{
		return Result;
	}

	const auto Channels = LinkInstance->Link.channels();
	Result.Reserve(static_cast<int32>(Channels.size()));

	for (const auto& Ch : Channels)
	{
		FLink4UEChannel& Out = Result.AddDefaulted_GetRef();
		Out.ChannelId = NodeIdToHex(Ch.id);
		Out.Name = UTF8_TO_TCHAR(Ch.name.c_str());
		Out.PeerId = NodeIdToHex(Ch.peerId);
		Out.PeerName = UTF8_TO_TCHAR(Ch.peerName.c_str());
	}

	return Result;
}
