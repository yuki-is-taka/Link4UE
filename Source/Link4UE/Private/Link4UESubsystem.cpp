// Copyright YUKITAKA. All Rights Reserved.

#include "Link4UESubsystem.h"
#include "Link4UESettings.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "ISubmixBufferListener.h"
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
		// ListenerName is set at construction and intentionally NOT updated on rename.
		// UnregisterSubmixBufferListener matches by pointer, not name.
		return ListenerName;
	}

	/** Rename the underlying LinkAudio Sink channel.
	 *  Must be called from GameThread only.
	 *  SDK marks setName() as "Thread-safe: no" but the internal implementation uses
	 *  util::Locked<> + atomic_flag, so GameThread → Link thread access is safe in practice.
	 *  Audio thread only touches retainBuffer()/commit() which are separate data paths. */
	void SetChannelName(const FString& NewName)
	{
		Sink.setName(TCHAR_TO_UTF8(*NewName));
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
		TWeakObjectPtr<USoundSubmix> TargetSubmix; // null = Master, used for diff matching
	};

	FLink4UEReceiveBridge(ableton::LinkAudio& InLink,
		const ableton::ChannelId& InChannelId,
		int32 InDeviceSampleRate,
		const FString& InChannelName)
		: ChannelIdHex(NodeIdToHex(InChannelId))
		, DeviceSampleRate(InDeviceSampleRate)
		, ChannelName(InChannelName)
		, Source(InLink, InChannelId,
			[this](ableton::LinkAudioSource::BufferHandle BufferHandle)
			{
				OnSourceBuffer(BufferHandle);
			})
	{
		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Receive bridge created — '%s' (id=%s, rate=%d)"),
			*ChannelName, *ChannelIdHex, InDeviceSampleRate);
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

	/** Update the device sample rate when UE's AudioDevice is recreated.
	 *  Flushes stale samples, updates ProceduralSound metadata, and re-registers
	 *  ActiveSounds with the new AudioDevice. LinkAudioSource is untouched —
	 *  no Link channel disruption. */
	void UpdateDeviceSampleRate(int32 NewRate, FAudioDevice* AudioDevice)
	{
		const int32 OldRate = DeviceSampleRate.exchange(NewRate, std::memory_order_relaxed);
		if (OldRate == NewRate)
		{
			return;
		}

		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Receive bridge '%s' sample rate changed — %d -> %d"),
			*ChannelName, OldRate, NewRate);

		for (FOutput& Out : Outputs)
		{
			if (!Out.ProceduralSound.IsValid())
			{
				continue;
			}

			// Flush samples resampled at the old rate
			Out.ProceduralSound->ResetAudio();
			Out.ProceduralSound->SetSampleRate(NewRate);

			// Re-register FActiveSound with the new AudioDevice
			// (the old device's ActiveSounds were destroyed with it)
			FActiveSound NewActiveSound;
			NewActiveSound.SetSound(Out.ProceduralSound.Get());
			NewActiveSound.SetWorld(nullptr);
			NewActiveSound.bAllowSpatialization = false;
			NewActiveSound.bIsUISound = true;
			NewActiveSound.bLocationDefined = false;

			if (Out.TargetSubmix.IsValid())
			{
				FSoundSubmixSendInfo SubmixSend;
				SubmixSend.SoundSubmix = Out.TargetSubmix.Get();
				SubmixSend.SendLevel = 1.0f;
				NewActiveSound.SetSubmixSend(SubmixSend);
			}

			AudioDevice->AddNewActiveSound(NewActiveSound);
		}
	}

	void AddOutput(USoundSubmix* InTargetSubmix, FAudioDevice* AudioDevice)
	{
		FOutput& Out = Outputs.AddDefaulted_GetRef();
		Out.TargetSubmix = InTargetSubmix;

		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>();
		Wave->SetSampleRate(DeviceSampleRate.load(std::memory_order_relaxed));
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

		if (InTargetSubmix)
		{
			FSoundSubmixSendInfo SubmixSend;
			SubmixSend.SoundSubmix = InTargetSubmix;
			SubmixSend.SendLevel = 1.0f;
			NewActiveSound.SetSubmixSend(SubmixSend);
		}

		AudioDevice->AddNewActiveSound(NewActiveSound);

		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Receive output added — '%s' -> Submix '%s'"),
			*ChannelName,
			InTargetSubmix ? *InTargetSubmix->GetName() : TEXT("Master"));
	}

	void RemoveOutput(int32 Index, FAudioDevice* AudioDevice)
	{
		if (!Outputs.IsValidIndex(Index))
		{
			return;
		}
		FOutput& Out = Outputs[Index];
		if (AudioDevice && Out.ProceduralSound.IsValid())
		{
			AudioDevice->StopSoundsUsingResource(Out.ProceduralSound.Get());
		}
		const FString SubmixName = Out.TargetSubmix.IsValid()
			? Out.TargetSubmix->GetName() : TEXT("Master");
		Outputs.RemoveAt(Index);
		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Receive output removed — '%s' -> Submix '%s'"),
			*ChannelName, *SubmixName);
	}

	int32 GetNumOutputs() const { return Outputs.Num(); }
	USoundSubmix* GetOutputSubmix(int32 Index) const
	{
		return Outputs.IsValidIndex(Index) ? Outputs[Index].TargetSubmix.Get() : nullptr;
	}

	const FString& GetChannelIdHex() const { return ChannelIdHex; }
	const FString& GetChannelName() const { return ChannelName; }
	void SetChannelName(const FString& NewName) { ChannelName = NewName; }

private:
	void OnSourceBuffer(ableton::LinkAudioSource::BufferHandle Handle)
	{
		const int32 SrcRate = static_cast<int32>(Handle.info.sampleRate);
		const int32 SrcFrames = static_cast<int32>(Handle.info.numFrames);
		const int32 SrcChannels = static_cast<int32>(Handle.info.numChannels);

		if (SrcFrames == 0 || SrcChannels == 0)
		{
			return;
		}

		const uint8* AudioData;
		int32 AudioBytes;

		const int32 DstRate = DeviceSampleRate.load(std::memory_order_relaxed);

		if (SrcRate == DstRate)
		{
			// No resampling needed — pass int16 data directly
			AudioData = reinterpret_cast<const uint8*>(Handle.samples);
			AudioBytes = SrcFrames * SrcChannels * sizeof(int16);
		}
		else
		{
			// Linear interpolation resampling (#1)
			const double Ratio = static_cast<double>(SrcRate) / static_cast<double>(DstRate);
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
	FString ChannelIdHex;
	std::atomic<int32> DeviceSampleRate;
	FString ChannelName;
	ableton::LinkAudioSource Source; // Must be last — destructor stops callback first

	TArray<int16> ResampleBuffer;
};

// ---------------------------------------------------------------------------
// Pimpl — hides ableton::LinkAudio from the header
// ---------------------------------------------------------------------------

struct ULink4UESubsystem::FLinkInstance
{
	ableton::LinkAudio Link;

	// Channel snapshot for change detection logging
	struct FChannelSnapshot
	{
		FString Id;   // NodeIdToHex
		FString Name;
		FString PeerName;
	};
	TArray<FChannelSnapshot> PreviousChannels;

	/** Log channel diff: what was added, removed, or renamed. */
	void LogChannelDiff()
	{
		const auto AllChannels = Link.channels();

		// Build current snapshot
		TArray<FChannelSnapshot> Current;
		Current.Reserve(AllChannels.size());
		for (const auto& Ch : AllChannels)
		{
			Current.Add({
				NodeIdToHex(Ch.id),
				FString(UTF8_TO_TCHAR(Ch.name.c_str())),
				FString(UTF8_TO_TCHAR(Ch.peerName.c_str()))
			});
		}

		// Detect removed channels
		for (const auto& Prev : PreviousChannels)
		{
			bool bFound = false;
			for (const auto& Cur : Current)
			{
				if (Cur.Id == Prev.Id)
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				UE_LOG(LogLink4UE, Log,
					TEXT("Link4UE: Channel removed — '%s' (peer='%s', id=%s)"),
					*Prev.Name, *Prev.PeerName, *Prev.Id);
			}
		}

		// Detect added and renamed channels
		for (const auto& Cur : Current)
		{
			const FChannelSnapshot* Prev = nullptr;
			for (const auto& P : PreviousChannels)
			{
				if (P.Id == Cur.Id)
				{
					Prev = &P;
					break;
				}
			}

			if (!Prev)
			{
				UE_LOG(LogLink4UE, Log,
					TEXT("Link4UE: Channel added — '%s' (peer='%s', id=%s)"),
					*Cur.Name, *Cur.PeerName, *Cur.Id);
			}
			else
			{
				if (Prev->Name != Cur.Name || Prev->PeerName != Cur.PeerName)
				{
					UE_LOG(LogLink4UE, Log,
						TEXT("Link4UE: Channel renamed — '%s' -> '%s' (peer='%s', id=%s)"),
						*Prev->Name, *Cur.Name, *Cur.PeerName, *Cur.Id);
				}
			}
		}

		PreviousChannels = MoveTemp(Current);
	}

	// Active send bridges (Submix → LinkAudio Sink)
	struct FActiveSend
	{
		TSharedPtr<FLink4UESendBridge, ESPMode::ThreadSafe> Bridge;
		TWeakObjectPtr<USoundSubmix> Submix;
		FString ChannelName;
		bool bMatched = false; // transient flag for diff
	};
	TArray<FActiveSend> ActiveSends;

	// Active receive bridges (LinkAudio Source → Submix via USoundWaveProcedural)
	TArray<TUniquePtr<FLink4UEReceiveBridge>> ActiveReceives;

	// Auto master send — always captures master submix output when Link Audio is enabled.
	// The SDK requires at least one Sink for peers to establish the audio return path.
	FActiveSend MasterSend;

	FLinkInstance(double BPM, const std::string& PeerName)
		: Link(BPM, PeerName)
	{
	}

	void TearDownMasterSend()
	{
		FAudioDevice* AudioDevice = GetMainAudioDevice();
		if (AudioDevice && MasterSend.Bridge.IsValid() && MasterSend.Submix.IsValid())
		{
			AudioDevice->UnregisterSubmixBufferListener(
				MasterSend.Bridge.ToSharedRef(), *MasterSend.Submix.Get());
		}
		MasterSend.Bridge.Reset();
		MasterSend.Submix.Reset();
	}

	void TearDownUserSends()
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

	void TearDownAllSends()
	{
		TearDownMasterSend();
		TearDownUserSends();
	}

	void TearDownReceives()
	{
		// Bridge destructors: Source unsubscribes → StopSoundsUsingResource → TStrongObjectPtr releases
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

	// Capture initial channel snapshot for diff logging
	LinkInstance->LogChannelDiff();

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
		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Shutting down (sends=%d, receives=%d)"),
			LinkInstance->ActiveSends.Num(), LinkInstance->ActiveReceives.Num());

		LinkInstance->TearDownReceives();
		LinkInstance->TearDownAllSends();
		LinkInstance->Link.enableLinkAudio(false);
		LinkInstance->Link.enable(false);
		delete LinkInstance;
		LinkInstance = nullptr;
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// AudioDevice creation callback
// ---------------------------------------------------------------------------

void ULink4UESubsystem::OnAudioDeviceCreated(Audio::FDeviceId DeviceId)
{
	if (!LinkInstance)
	{
		return;
	}

	FAudioDevice* AudioDevice = GetMainAudioDevice();
	if (!AudioDevice)
	{
		return;
	}

	UE_LOG(LogLink4UE, Log,
		TEXT("Link4UE: Audio device created (id=%u)"), DeviceId);

	// Rebuild send routes (re-registers SubmixBufferListeners with new device)
	if (bSendRoutesPending)
	{
		RebuildAudioSends(GetDefault<ULink4UESettings>());
	}

	// Update receive bridges in-place — no LinkAudioSource destruction,
	// so Link channels stay connected without interruption.
	const int32 NewRate = AudioDevice->GetSampleRate();
	for (auto& Recv : LinkInstance->ActiveReceives)
	{
		Recv->UpdateDeviceSampleRate(NewRate, AudioDevice);
	}
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

	UE_LOG(LogLink4UE, Log,
		TEXT("Link4UE: Settings applied — tempo=%.1f, connect=%s, audio=%s, peer='%s'"),
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

	if (!Settings->bEnableLinkAudio)
	{
		LinkInstance->TearDownAllSends();
		bSendRoutesPending = false;
		return;
	}

	FAudioDevice* AudioDevice = GetMainAudioDevice();
	if (!AudioDevice)
	{
		bSendRoutesPending = true;
		UE_LOG(LogLink4UE, Warning,
			TEXT("Link4UE: Audio device not ready, send routes deferred"));
		return;
	}

	// Ensure master send exists (required by SDK for peers to establish audio return paths).
	if (!LinkInstance->MasterSend.Bridge.IsValid())
	{
		USoundSubmix& MasterSubmix = AudioDevice->GetMainSubmixObject();
		FString MasterChannelName = TEXT("Main");

		TSharedRef<FLink4UESendBridge, ESPMode::ThreadSafe> Bridge =
			MakeShared<FLink4UESendBridge, ESPMode::ThreadSafe>(
				LinkInstance->Link, MasterChannelName, kDefaultMaxSamples, Quantum);

		AudioDevice->RegisterSubmixBufferListener(Bridge, MasterSubmix);

		LinkInstance->MasterSend.Bridge = Bridge;
		LinkInstance->MasterSend.Submix = &MasterSubmix;

		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Master send created — Submix '%s' -> Sink '%s'"),
			*MasterSubmix.GetName(), *MasterChannelName);
	}

	// --- Diff user sends (2-pass matching) ---

	// Build desired list from settings
	struct FDesiredSend { USoundSubmix* Submix; FString ChannelName; };
	TArray<FDesiredSend> Desired;
	for (const FLink4UEAudioSend& SendDef : Settings->AudioSends)
	{
		USoundSubmix* Submix = SendDef.Submix.LoadSynchronous();
		if (!Submix)
		{
			// Empty Submix → fall back to Main Submix
			Submix = &AudioDevice->GetMainSubmixObject();
		}
		FString ChannelName = SendDef.ChannelNamePrefix.IsEmpty()
			? Submix->GetName()
			: SendDef.ChannelNamePrefix;
		Desired.Add({Submix, ChannelName});
	}

	// Clear match flags
	for (auto& Send : LinkInstance->ActiveSends)
	{
		Send.bMatched = false;
	}

	// Pass 1: Exact match (Submix + ChannelName) — stable bridges stay untouched
	TArray<int32> UnmatchedDesired;
	for (int32 Di = 0; Di < Desired.Num(); ++Di)
	{
		bool bFound = false;
		for (auto& Send : LinkInstance->ActiveSends)
		{
			if (!Send.bMatched
				&& Send.Submix.Get() == Desired[Di].Submix
				&& Send.ChannelName == Desired[Di].ChannelName)
			{
				Send.bMatched = true;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			UnmatchedDesired.Add(Di);
		}
	}

	// Pass 2: Submix-only match for remaining — rename in-place via Sink.setName()
	TArray<int32> StillUnmatched;
	for (int32 Di : UnmatchedDesired)
	{
		bool bFound = false;
		for (auto& Send : LinkInstance->ActiveSends)
		{
			if (!Send.bMatched && Send.Submix.Get() == Desired[Di].Submix)
			{
				Send.bMatched = true;
				bFound = true;

				// In-place rename — preserves Sink identity (ChannelId) so peers stay connected
				UE_LOG(LogLink4UE, Log,
					TEXT("Link4UE: Send renamed — '%s' -> '%s' (Submix '%s')"),
					*Send.ChannelName, *Desired[Di].ChannelName, *Desired[Di].Submix->GetName());
				Send.Bridge->SetChannelName(Desired[Di].ChannelName);
				Send.ChannelName = Desired[Di].ChannelName;
				break;
			}
		}
		if (!bFound)
		{
			StillUnmatched.Add(Di);
		}
	}

	// Remove unmatched active sends (reverse iteration for stable indices)
	for (int32 i = LinkInstance->ActiveSends.Num() - 1; i >= 0; --i)
	{
		if (!LinkInstance->ActiveSends[i].bMatched)
		{
			auto& Send = LinkInstance->ActiveSends[i];
			if (Send.Bridge.IsValid() && Send.Submix.IsValid())
			{
				AudioDevice->UnregisterSubmixBufferListener(
					Send.Bridge.ToSharedRef(), *Send.Submix.Get());
			}
			UE_LOG(LogLink4UE, Log,
				TEXT("Link4UE: Send removed — '%s' (Submix '%s')"),
				*Send.ChannelName,
				Send.Submix.IsValid() ? *Send.Submix->GetName() : TEXT("?"));
			LinkInstance->ActiveSends.RemoveAt(i);
		}
	}

	// Add still-unmatched desired sends (new Submix entries)
	for (int32 Di : StillUnmatched)
	{
		const FDesiredSend& D = Desired[Di];

		TSharedRef<FLink4UESendBridge, ESPMode::ThreadSafe> Bridge =
			MakeShared<FLink4UESendBridge, ESPMode::ThreadSafe>(
				LinkInstance->Link, D.ChannelName, kDefaultMaxSamples, Quantum);

		AudioDevice->RegisterSubmixBufferListener(Bridge, *D.Submix);

		FLinkInstance::FActiveSend& ActiveSend = LinkInstance->ActiveSends.AddDefaulted_GetRef();
		ActiveSend.Bridge = Bridge;
		ActiveSend.Submix = D.Submix;
		ActiveSend.ChannelName = D.ChannelName;

		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Send created — Submix '%s' -> Sink '%s'"),
			*D.Submix->GetName(), *D.ChannelName);
	}

	bSendRoutesPending = false;
}

void ULink4UESubsystem::RebuildAudioReceives(const ULink4UESettings* Settings)
{
	if (!LinkInstance || !Settings)
	{
		return;
	}

	if (!Settings->bEnableLinkAudio)
	{
		LinkInstance->TearDownReceives();
		bReceiveRoutesPending = false;
		return;
	}

	FAudioDevice* AudioDevice = GetMainAudioDevice();
	if (!AudioDevice)
	{
		bReceiveRoutesPending = true;
		UE_LOG(LogLink4UE, Warning,
			TEXT("Link4UE: Audio device not ready, receive routes deferred"));
		return;
	}

	const auto AllChannels = LinkInstance->Link.channels();
	using FChannelInfo = std::remove_reference_t<decltype(AllChannels)>::value_type;

	// Build ID lookup from current session
	TMap<FString, const FChannelInfo*> SessionById;
	TMap<FString, const FChannelInfo*> SessionByName;
	for (const auto& Ch : AllChannels)
	{
		SessionById.Add(NodeIdToHex(Ch.id), &Ch);
		SessionByName.Add(FString(UTF8_TO_TCHAR(Ch.name.c_str())), &Ch);
	}

	const int32 DeviceSampleRate = AudioDevice->GetSampleRate();

	// --- Build desired state keyed by ChannelId ---
	struct FDesiredOutput { USoundSubmix* Submix; };
	struct FDesiredChannel
	{
		ableton::ChannelId Id;
		FString Name;
		TArray<FDesiredOutput> Outputs;
	};
	TMap<FString, FDesiredChannel> DesiredMap; // Key = ChannelIdHex

	for (const FLink4UEAudioReceive& RecvDef : Settings->AudioReceives)
	{
		if (RecvDef.ChannelId.IsEmpty() && RecvDef.ChannelName.IsEmpty())
		{
			continue;
		}

		// Resolve: prefer ID, fall back to name
		const FChannelInfo* Found = nullptr;
		if (!RecvDef.ChannelId.IsEmpty())
		{
			auto* Ptr = SessionById.Find(RecvDef.ChannelId);
			if (Ptr) Found = *Ptr;
		}
		if (!Found && !RecvDef.ChannelName.IsEmpty())
		{
			auto* Ptr = SessionByName.Find(RecvDef.ChannelName);
			if (Ptr) Found = *Ptr;
		}

		if (!Found)
		{
			UE_LOG(LogLink4UE, Log,
				TEXT("Link4UE: Receive skipped — channel '%s' not online (id=%s)"),
				*RecvDef.ChannelName, *RecvDef.ChannelId);
			continue;
		}

		const FString IdHex = NodeIdToHex(Found->id);
		FDesiredChannel& DC = DesiredMap.FindOrAdd(IdHex);
		DC.Id = Found->id;
		DC.Name = FString(UTF8_TO_TCHAR(Found->name.c_str()));
		USoundSubmix* Submix = RecvDef.Submix.LoadSynchronous();
		DC.Outputs.Add({Submix});
	}

	// --- Diff against active bridges (keyed by ChannelIdHex) ---

	// 1. Remove bridges whose channel ID is no longer desired
	for (int32 i = LinkInstance->ActiveReceives.Num() - 1; i >= 0; --i)
	{
		const FString& IdHex = LinkInstance->ActiveReceives[i]->GetChannelIdHex();
		if (!DesiredMap.Contains(IdHex))
		{
			UE_LOG(LogLink4UE, Log,
				TEXT("Link4UE: Receive bridge removed — '%s' (id=%s)"),
				*LinkInstance->ActiveReceives[i]->GetChannelName(), *IdHex);
			LinkInstance->ActiveReceives.RemoveAt(i);
		}
	}

	// 2. For each desired channel, find or create bridge, then diff outputs
	for (auto& Pair : DesiredMap)
	{
		const FString& IdHex = Pair.Key;
		FDesiredChannel& DC = Pair.Value;

		// Find existing bridge by ID
		FLink4UEReceiveBridge* Bridge = nullptr;
		for (auto& Recv : LinkInstance->ActiveReceives)
		{
			if (Recv->GetChannelIdHex() == IdHex)
			{
				Bridge = Recv.Get();
				break;
			}
		}

		if (!Bridge)
		{
			LinkInstance->ActiveReceives.Add(MakeUnique<FLink4UEReceiveBridge>(
				LinkInstance->Link, DC.Id, DeviceSampleRate, DC.Name));
			Bridge = LinkInstance->ActiveReceives.Last().Get();
		}
		else
		{
			// Update display name if it changed (peer renamed channel)
			if (Bridge->GetChannelName() != DC.Name)
			{
				UE_LOG(LogLink4UE, Log,
					TEXT("Link4UE: Receive bridge renamed — '%s' -> '%s' (id=%s)"),
					*Bridge->GetChannelName(), *DC.Name, *IdHex);
				Bridge->SetChannelName(DC.Name);
			}
		}

		// Diff outputs within this bridge
		TArray<bool> ActiveMatched;
		ActiveMatched.SetNumZeroed(Bridge->GetNumOutputs());

		TArray<int32> UnmatchedDesired;
		for (int32 Di = 0; Di < DC.Outputs.Num(); ++Di)
		{
			bool bFound = false;
			for (int32 Ai = 0; Ai < Bridge->GetNumOutputs(); ++Ai)
			{
				if (!ActiveMatched[Ai]
					&& Bridge->GetOutputSubmix(Ai) == DC.Outputs[Di].Submix)
				{
					ActiveMatched[Ai] = true;
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				UnmatchedDesired.Add(Di);
			}
		}

		for (int32 Ai = ActiveMatched.Num() - 1; Ai >= 0; --Ai)
		{
			if (!ActiveMatched[Ai])
			{
				Bridge->RemoveOutput(Ai, AudioDevice);
			}
		}

		for (int32 Di : UnmatchedDesired)
		{
			Bridge->AddOutput(DC.Outputs[Di].Submix, AudioDevice);
		}

		if (Bridge->GetNumOutputs() == 0)
		{
			for (int32 i = LinkInstance->ActiveReceives.Num() - 1; i >= 0; --i)
			{
				if (LinkInstance->ActiveReceives[i].Get() == Bridge)
				{
					LinkInstance->ActiveReceives.RemoveAt(i);
					break;
				}
			}
		}
	}

	// bReceiveRoutesPending is only for AudioDevice unavailability — not for missing channels
	bReceiveRoutesPending = false;
}

void ULink4UESubsystem::SyncChannelNames()
{
	if (!LinkInstance)
	{
		return;
	}

	const auto AllChannels = LinkInstance->Link.channels();
	ULink4UESettings* MutableSettings = GetMutableDefault<ULink4UESettings>();
	bool bAnyChanged = false;

	for (FLink4UEAudioReceive& Recv : MutableSettings->AudioReceives)
	{
		if (Recv.ChannelId.IsEmpty())
		{
			continue;
		}

		for (const auto& Ch : AllChannels)
		{
			if (NodeIdToHex(Ch.id) == Recv.ChannelId)
			{
				const FString CurrentName = FString(UTF8_TO_TCHAR(Ch.name.c_str()));
				if (Recv.ChannelName != CurrentName)
				{
					UE_LOG(LogLink4UE, Log,
						TEXT("Link4UE: Channel name synced — '%s' -> '%s' (id=%s)"),
						*Recv.ChannelName, *CurrentName, *Recv.ChannelId);
					Recv.ChannelName = CurrentName;
					bAnyChanged = true;
				}
				break;
			}
		}
	}

	if (bAnyChanged)
	{
		MutableSettings->SaveConfig();
	}
}

#if WITH_EDITOR
void ULink4UESubsystem::OnSettingsChanged(FName PropertyName)
{
	const ULink4UESettings* Settings = GetDefault<ULink4UESettings>();
	if (!LinkInstance || !Settings)
	{
		return;
	}

	// Audio routing properties — rebuild only the affected side
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ULink4UESettings, AudioSends))
	{
		bIsRebuilding = true;
		RebuildAudioSends(Settings);
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
		return;
	}
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ULink4UESettings, AudioReceives))
	{
		bIsRebuilding = true;
		RebuildAudioReceives(Settings);
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
		return;
	}

	// Everything else — full apply
	ApplySettings(Settings);
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
		LinkInstance->LogChannelDiff();
		if (!bIsRebuilding)
		{
			UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Channels changed, rebuilding receive routes"));
			bIsRebuilding = true;
			SyncChannelNames();
			RebuildAudioReceives(GetDefault<ULink4UESettings>());
			bIsRebuilding = false;
			bChannelsDirty.store(false, std::memory_order_relaxed);
		}
		OnChannelsChanged.Broadcast();
	}

	// Retry deferred audio routes once AudioDevice becomes available
	if (bSendRoutesPending || bReceiveRoutesPending)
	{
		FAudioDevice* AudioDevice = GetMainAudioDevice();
		if (AudioDevice)
		{
			const ULink4UESettings* Settings = GetDefault<ULink4UESettings>();
			bIsRebuilding = true;
			if (bSendRoutesPending)
			{
				UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Audio device ready, rebuilding deferred send routes"));
				RebuildAudioSends(Settings);
			}
			if (bReceiveRoutesPending)
			{
				UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Audio device ready, rebuilding deferred receive routes"));
				RebuildAudioReceives(Settings);
			}
			bIsRebuilding = false;
			bChannelsDirty.store(false, std::memory_order_relaxed);
		}
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
		UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Link enabled"));
	}
}

void ULink4UESubsystem::DisableLink()
{
	if (LinkInstance)
	{
		LinkInstance->Link.enable(false);
		UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Link disabled"));
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
		UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Link Audio %s"), bEnable ? TEXT("enabled") : TEXT("disabled"));
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
