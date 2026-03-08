// Copyright YUKITAKA. All Rights Reserved.

#include "Link4UESubsystem.h"
#include "Link4UESettings.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "ISubmixBufferListener.h"
#include "AudioMixerDevice.h"
#include "Sound/SoundWaveProcedural.h"
#include "ActiveSound.h"
#include "ableton/LinkAudio.hpp"
#include "ableton/util/FloatIntConversion.hpp"

namespace
{
	constexpr size_t kDefaultMaxSamples = 8192;

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

		// Ensure Sink buffer is large enough (cached to avoid per-callback API call)
		const size_t RequiredSamples = static_cast<size_t>(TotalLinkSamples);
		if (RequiredSamples > CachedMaxSamples)
		{
			Sink.requestMaxNumSamples(RequiredSamples);
			CachedMaxSamples = RequiredSamples;
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
	size_t CachedMaxSamples = 0;
};

// ---------------------------------------------------------------------------
// FLink4UEReceiveBridge — receives LinkAudio Source data and routes to Submix
//   via USoundWaveProcedural + FActiveSound (no UAudioComponent needed)
// ---------------------------------------------------------------------------

class FLink4UEReceiveBridge
{
public:
	struct FOutput
	{
		TStrongObjectPtr<USoundWaveProcedural> ProceduralSound;
	};

	FLink4UEReceiveBridge(ableton::LinkAudio& InLink,
		const ableton::ChannelId& InChannelId,
		int32 InDeviceSampleRate,
		const FString& InChannelName)
		: DeviceSampleRate(InDeviceSampleRate)
		, ChannelName(InChannelName)
		, CreationTime(FPlatformTime::Seconds())
		, Source(InLink, InChannelId,
			[this](ableton::LinkAudioSource::BufferHandle BufferHandle)
			{
				OnSourceBuffer(BufferHandle);
			})
	{
		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: ReceiveBridge constructed for '%s' (channelId=%s, deviceRate=%d)"),
			*ChannelName, *NodeIdToHex(InChannelId), DeviceSampleRate);
	}

	~FLink4UEReceiveBridge()
	{
		// Teardown order (#8):
		// 1. Source destructor fires first (member destruction order is reverse)
		//    → SDK callback stops, no more QueueAudio calls
		// 2. Stop FActiveSounds via StopSoundsUsingResource
		//    → GeneratePCMData stops being called
		// 3. TStrongObjectPtr releases USoundWaveProcedural
		FAudioDevice* AudioDevice = GetMainAudioDevice();
		for (FOutput& Out : Outputs)
		{
			if (AudioDevice && Out.ProceduralSound.IsValid())
			{
				AudioDevice->StopSoundsUsingResource(Out.ProceduralSound.Get());
			}
			// TStrongObjectPtr::Reset in destructor releases the UObject
		}
	}

	void AddOutput(USoundSubmix* TargetSubmix, FAudioDevice* AudioDevice)
	{
		FOutput& Out = Outputs.AddDefaulted_GetRef();

		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>();
		Wave->SetSampleRate(DeviceSampleRate);
		Wave->NumChannels = 2; // Link Audio is stereo in practice
		Wave->Duration = INDEFINITELY_LOOPING_DURATION;
		Wave->SoundGroup = SOUNDGROUP_Default;
		Wave->bLooping = true;
		Out.ProceduralSound.Reset(Wave);

		FActiveSound NewActiveSound;
		NewActiveSound.SetSound(Wave);
		NewActiveSound.SetWorld(nullptr);
		NewActiveSound.bAllowSpatialization = false;
		NewActiveSound.bIsUISound = true;
		NewActiveSound.bLocationDefined = false;

		if (TargetSubmix)
		{
			FSoundSubmixSendInfo SubmixSend;
			SubmixSend.SoundSubmix = TargetSubmix;
			SubmixSend.SendLevel = 1.0f;
			NewActiveSound.SetSubmixSend(SubmixSend);
		}

		AudioDevice->AddNewActiveSound(NewActiveSound);

		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: ReceiveBridge '%s' output added → Submix '%s'"),
			*ChannelName,
			TargetSubmix ? *TargetSubmix->GetName() : TEXT("(Master)"));
	}

	const FString& GetChannelName() const { return ChannelName; }
	uint64 GetCallbackCount() const { return CallbackCount.load(std::memory_order_relaxed); }
	double GetCreationTime() const { return CreationTime; }
	bool HasReceivedCallback() const { return bLoggedFirstCallback; }
	uint32 GetOverrunCount() const { return 0; }
	uint32 GetUnderrunCount() const { return 0; }

private:
	void OnSourceBuffer(ableton::LinkAudioSource::BufferHandle Handle)
	{
		CallbackCount.fetch_add(1, std::memory_order_relaxed);

		const int32 SrcRate = static_cast<int32>(Handle.info.sampleRate);
		const int32 SrcFrames = static_cast<int32>(Handle.info.numFrames);
		const int32 SrcChannels = static_cast<int32>(Handle.info.numChannels);

		if (SrcFrames == 0 || SrcChannels == 0)
		{
			return;
		}

		if (!bLoggedFirstCallback)
		{
			bLoggedFirstCallback = true;
			UE_LOG(LogLink4UE, Warning,
				TEXT("Link4UE Receive: first callback for '%s' (frames=%d, ch=%d, rate=%d)"),
				*ChannelName, SrcFrames, SrcChannels, SrcRate);
		}

		const uint8* AudioData;
		int32 AudioBytes;

		if (SrcRate == DeviceSampleRate)
		{
			// No resampling needed — pass int16 data directly
			AudioData = reinterpret_cast<const uint8*>(Handle.samples);
			AudioBytes = SrcFrames * SrcChannels * sizeof(int16);
		}
		else
		{
			// Linear interpolation resampling (#1)
			const double Ratio = static_cast<double>(SrcRate) / static_cast<double>(DeviceSampleRate);
			const int32 DstFrames = static_cast<int32>(SrcFrames / Ratio);
			ResampleBuffer.SetNumUninitialized(DstFrames * SrcChannels, EAllowShrinking::No);

			for (int32 DstFrame = 0; DstFrame < DstFrames; ++DstFrame)
			{
				const double SrcPos = DstFrame * Ratio;
				const int32 Idx0 = static_cast<int32>(SrcPos);
				const int32 Idx1 = FMath::Min(Idx0 + 1, SrcFrames - 1);
				const double Frac = SrcPos - Idx0;

				for (int32 Ch = 0; Ch < SrcChannels; ++Ch)
				{
					const int16 S0 = Handle.samples[Idx0 * SrcChannels + Ch];
					const int16 S1 = Handle.samples[Idx1 * SrcChannels + Ch];
					ResampleBuffer[DstFrame * SrcChannels + Ch] =
						static_cast<int16>(S0 + static_cast<int16>((S1 - S0) * Frac));
				}
			}

			AudioData = reinterpret_cast<const uint8*>(ResampleBuffer.GetData());
			AudioBytes = DstFrames * SrcChannels * sizeof(int16);
		}

		// Push to all outputs (#7 — one Bridge, multiple outputs)
		for (FOutput& Out : Outputs)
		{
			if (Out.ProceduralSound.IsValid())
			{
				Out.ProceduralSound->QueueAudio(AudioData, AudioBytes);
			}
		}
	}

	TArray<FOutput> Outputs;
	int32 DeviceSampleRate;
	FString ChannelName;
	double CreationTime;
	ableton::LinkAudioSource Source; // Must be last — destructor stops callback first

	TArray<int16> ResampleBuffer;
	std::atomic<bool> bLoggedFirstCallback{false};
	std::atomic<uint64> CallbackCount{0};
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

	// Active receive bridges (LinkAudio Source → AudioBus)
	TArray<TUniquePtr<FLink4UEReceiveBridge>> ActiveReceives;

	// Auto master send — always captures master submix output when Link Audio is enabled.
	// The SDK requires at least one Sink for peers to establish the audio return path.
	FActiveSend MasterSend;

	float HealthCheckTimer = 0.f;

	FLinkInstance(double BPM, const std::string& PeerName)
		: Link(BPM, PeerName)
	{
	}

	void HealthCheckTick(float DeltaTime)
	{
		HealthCheckTimer += DeltaTime;
		if (HealthCheckTimer < 5.0f)
		{
			return;
		}
		HealthCheckTimer = 0.f;

		const double Now = FPlatformTime::Seconds();
		const bool bLinkEnabled = Link.isEnabled();
		const bool bAudioEnabled = Link.isLinkAudioEnabled();
		const auto AllChannels = Link.channels();
		const int32 NumPeers = static_cast<int32>(Link.numPeers());

		const bool bHasMasterSend = MasterSend.Bridge.IsValid();
		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE Health: peers=%d, link=%s, audio=%s, channels=%d, masterSend=%s, sends=%d, receives=%d"),
			NumPeers,
			bLinkEnabled ? TEXT("ON") : TEXT("OFF"),
			bAudioEnabled ? TEXT("ON") : TEXT("OFF"),
			static_cast<int32>(AllChannels.size()),
			bHasMasterSend ? TEXT("YES") : TEXT("NO"),
			ActiveSends.Num(),
			ActiveReceives.Num());

		for (int32 i = 0; i < ActiveReceives.Num(); ++i)
		{
			const auto& Recv = ActiveReceives[i];
			const double Age = Now - Recv->GetCreationTime();
			UE_LOG(LogLink4UE, Log,
				TEXT("  Receive[%d] '%s': callbacks=%llu, age=%.1fs, hasReceived=%s, overruns=%u, underruns=%u"),
				i, *Recv->GetChannelName(),
				Recv->GetCallbackCount(),
				Age,
				Recv->HasReceivedCallback() ? TEXT("YES") : TEXT("NO"),
				Recv->GetOverrunCount(),
				Recv->GetUnderrunCount());
		}
	}

	void TearDownSends()
	{
		FAudioDevice* AudioDevice = GetMainAudioDevice();

		// Tear down master send
		if (AudioDevice && MasterSend.Bridge.IsValid() && MasterSend.Submix.IsValid())
		{
			AudioDevice->UnregisterSubmixBufferListener(
				MasterSend.Bridge.ToSharedRef(), *MasterSend.Submix.Get());
		}
		MasterSend.Bridge.Reset();
		MasterSend.Submix.Reset();

		// Tear down user-configured sends
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

	// Listen for AudioDevice creation so we can rebuild routes once the device is ready
	AudioDeviceCreatedHandle = FAudioDeviceManagerDelegates::OnAudioDeviceCreated.AddUObject(
		this, &ULink4UESubsystem::OnAudioDeviceCreated);

#if WITH_EDITOR
	SettingsChangedHandle = ULink4UESettings::OnSettingsChanged.AddUObject(
		this, &ULink4UESubsystem::OnSettingsChanged);
#endif
}

void ULink4UESubsystem::Deinitialize()
{
	FAudioDeviceManagerDelegates::OnAudioDeviceCreated.Remove(AudioDeviceCreatedHandle);

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
// AudioDevice creation callback
// ---------------------------------------------------------------------------

void ULink4UESubsystem::OnAudioDeviceCreated(Audio::FDeviceId DeviceId)
{
	// Only rebuild if sends are actually pending — this callback can fire multiple times
	if (!bSendRoutesPending)
	{
		return;
	}
	UE_LOG(LogLink4UE, Log,
		TEXT("Link4UE: OnAudioDeviceCreated (id=%u) — rebuilding deferred send routes"), DeviceId);
	RebuildAudioSends(GetDefault<ULink4UESettings>());
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

	bIsRebuilding = true;
	RebuildAudioSends(Settings);
	RebuildAudioReceives(Settings);
	bIsRebuilding = false;
	// Drain any channel-dirty flags caused by our own Sink/Source creation/destruction
	bChannelsDirty.store(false, std::memory_order_relaxed);

	UE_LOG(LogLink4UE, Log, TEXT("Link4UE settings applied (tempo=%.1f, auto=%s, audio=%s, peer=%s)"),
		Settings->DefaultTempo,
		Settings->bAutoConnect ? TEXT("on") : TEXT("off"),
		Settings->bEnableLinkAudio ? TEXT("on") : TEXT("off"),
		*Settings->PeerName);
}

void ULink4UESubsystem::RebuildAudioSends(const ULink4UESettings* Settings)
{
	if (!LinkInstance || !Settings)
	{
		return;
	}

	LinkInstance->TearDownSends();

	if (!Settings->bEnableLinkAudio)
	{
		bSendRoutesPending = false;
		return;
	}

	FAudioDevice* AudioDevice = GetMainAudioDevice();
	if (!AudioDevice)
	{
		bSendRoutesPending = true;
		UE_LOG(LogLink4UE, Warning,
			TEXT("Link4UE: AudioDevice not available — send routes deferred"));
		return;
	}

	// Always create a Sink for the master submix output.
	// This is required by the SDK for peers to establish audio return paths,
	// and makes UE's master output available as a channel on the Link Audio network.
	{
		USoundSubmix& MasterSubmix = AudioDevice->GetMainSubmixObject();
		FString MasterChannelName = Settings->PeerName;

		TSharedRef<FLink4UESendBridge, ESPMode::ThreadSafe> Bridge =
			MakeShared<FLink4UESendBridge, ESPMode::ThreadSafe>(
				LinkInstance->Link, MasterChannelName, kDefaultMaxSamples, Quantum);

		AudioDevice->RegisterSubmixBufferListener(Bridge, MasterSubmix);

		LinkInstance->MasterSend.Bridge = Bridge;
		LinkInstance->MasterSend.Submix = &MasterSubmix;

		UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Master send created [%s] → Sink '%s'"),
			*MasterSubmix.GetName(), *MasterChannelName);
	}

	// User-configured additional sends
	for (const FLink4UEAudioSend& SendDef : Settings->AudioSends)
	{
		USoundSubmix* Submix = SendDef.Submix.LoadSynchronous();
		if (!Submix)
		{
			UE_LOG(LogLink4UE, Warning,
				TEXT("Link4UE: AudioSend skipped — Submix asset not found"));
			continue;
		}

		FString ChannelName = SendDef.ChannelNamePrefix.IsEmpty()
			? Submix->GetName()
			: SendDef.ChannelNamePrefix;

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

	bSendRoutesPending = false;
}

void ULink4UESubsystem::RebuildAudioReceives(const ULink4UESettings* Settings)
{
	if (!LinkInstance || !Settings)
	{
		return;
	}

	LinkInstance->TearDownReceives();

	if (!Settings->bEnableLinkAudio)
	{
		bReceiveRoutesPending = false;
		return;
	}

	FAudioDevice* AudioDevice = GetMainAudioDevice();
	if (!AudioDevice)
	{
		bReceiveRoutesPending = true;
		UE_LOG(LogLink4UE, Warning,
			TEXT("Link4UE: AudioDevice not available — receive routes deferred"));
		return;
	}

	const auto AllChannels = LinkInstance->Link.channels();

	UE_LOG(LogLink4UE, Log, TEXT("Link4UE: %d channel(s) visible in session:"),
		static_cast<int32>(AllChannels.size()));
	for (const auto& Ch : AllChannels)
	{
		UE_LOG(LogLink4UE, Log, TEXT("  - '%s' (peer='%s', id=%s)"),
			UTF8_TO_TCHAR(Ch.name.c_str()),
			UTF8_TO_TCHAR(Ch.peerName.c_str()),
			*NodeIdToHex(Ch.id));
	}

	bool bAnyValidChannelMissing = false;

	const int32 DeviceSampleRate = AudioDevice->GetSampleRate();

	// Group settings entries by ChannelName → 1 Bridge per channel (#7)
	TMap<FString, FLink4UEReceiveBridge*> BridgeMap;

	for (const FLink4UEAudioReceive& RecvDef : Settings->AudioReceives)
	{
		if (RecvDef.ChannelName.IsEmpty())
		{
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
			bAnyValidChannelMissing = true;
			UE_LOG(LogLink4UE, Warning,
				TEXT("Link4UE: AudioReceive skipped — channel '%s' not found in session"),
				*RecvDef.ChannelName);
			continue;
		}

		// Reuse existing Bridge for same channel, or create new one
		FLink4UEReceiveBridge*& Bridge = BridgeMap.FindOrAdd(RecvDef.ChannelName);
		if (!Bridge)
		{
			LinkInstance->ActiveReceives.Add(MakeUnique<FLink4UEReceiveBridge>(
				LinkInstance->Link, *FoundId, DeviceSampleRate, RecvDef.ChannelName));
			Bridge = LinkInstance->ActiveReceives.Last().Get();
		}

		USoundSubmix* Submix = RecvDef.Submix.LoadSynchronous();
		// null Submix = Master Submix (FActiveSound default routing)
		Bridge->AddOutput(Submix, AudioDevice);
	}

	bReceiveRoutesPending = bAnyValidChannelMissing;
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
	bool bRebuiltReceivesThisTick = false;
	if (bChannelsDirty.exchange(false, std::memory_order_acquire))
	{
		if (!bIsRebuilding)
		{
			UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Channels changed — rebuilding receive routes"));
			bIsRebuilding = true;
			RebuildAudioReceives(GetDefault<ULink4UESettings>());
			bIsRebuilding = false;
			// Drain self-triggered dirty flags
			bChannelsDirty.store(false, std::memory_order_relaxed);
			bRebuiltReceivesThisTick = true;
		}
		OnChannelsChanged.Broadcast();
	}

	// Retry deferred audio routes once AudioDevice becomes available
	if (bSendRoutesPending || (bReceiveRoutesPending && !bRebuiltReceivesThisTick))
	{
		FAudioDevice* AudioDevice = GetMainAudioDevice();
		if (AudioDevice)
		{
			const ULink4UESettings* Settings = GetDefault<ULink4UESettings>();
			bIsRebuilding = true;
			if (bSendRoutesPending)
			{
				UE_LOG(LogLink4UE, Log, TEXT("Link4UE: AudioDevice now available — retrying deferred send routes"));
				RebuildAudioSends(Settings);
			}
			if (bReceiveRoutesPending)
			{
				UE_LOG(LogLink4UE, Log, TEXT("Link4UE: AudioDevice now available — retrying deferred receive routes"));
				RebuildAudioReceives(Settings);
			}
			bIsRebuilding = false;
			// Drain self-triggered dirty flags
			bChannelsDirty.store(false, std::memory_order_relaxed);
		}
	}

	// Periodic health check for receive diagnostics
	LinkInstance->HealthCheckTick(DeltaTime);

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
