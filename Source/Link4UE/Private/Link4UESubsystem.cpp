// Copyright YUKITAKA. All Rights Reserved.

#include "Link4UESubsystem.h"
#if WITH_EDITOR
#include "ISettingsModule.h"
#endif
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

	FAudioDevice* GetActiveDevice()
	{
		if (FAudioDeviceManager* ADM = FAudioDeviceManager::Get())
		{
			return ADM->GetActiveAudioDevice().GetAudioDevice();
		}
		return nullptr;
	}

	FAudioDevice* GetDeviceById(Audio::FDeviceId DeviceId)
	{
		if (DeviceId == INDEX_NONE) return nullptr;
		if (FAudioDeviceManager* ADM = FAudioDeviceManager::Get())
		{
			return ADM->GetAudioDevice(DeviceId).GetAudioDevice();
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
//
//   Active-device-only design: all outputs play on a single device at a time.
//   SwitchToDevice() stops the old device's sounds and recreates on the new one.
// ---------------------------------------------------------------------------

class FLink4UEReceiveBridge
{
public:
	/** Logical output (one per TargetSubmix). */
	struct FOutput
	{
		TWeakObjectPtr<USoundSubmix> TargetSubmix; // null = Master, used for diff matching
		TStrongObjectPtr<USoundWaveProcedural> ProceduralSound;
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
		// Teardown order:
		// 1. Source destructor fires first (member destruction order is reverse)
		//    → SDK callback stops, no more QueueAudio calls
		// 2. Stop FActiveSounds on the active device
		// 3. TStrongObjectPtr releases USoundWaveProcedural
		FScopeLock Lock(&OutputsLock);
		DeactivateLocked();
	}

	/** Add a logical output and create playback on the given device. */
	void AddOutput(USoundSubmix* InTargetSubmix, FAudioDevice* Device)
	{
		FScopeLock Lock(&OutputsLock);
		FOutput& Out = Outputs.AddDefaulted_GetRef();
		Out.TargetSubmix = InTargetSubmix;

		if (Device)
		{
			CreateProceduralSound(Out, Device);
			ActiveDeviceId = Device->DeviceID;
		}

		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Receive output added — '%s' -> Submix '%s' (device=%u)"),
			*ChannelName,
			InTargetSubmix ? *InTargetSubmix->GetName() : TEXT("Master"),
			ActiveDeviceId);
	}

	/** Remove a logical output. */
	void RemoveOutput(int32 Index)
	{
		if (!Outputs.IsValidIndex(Index))
		{
			return;
		}
		FScopeLock Lock(&OutputsLock);
		FOutput& Out = Outputs[Index];
		const FString SubmixName = Out.TargetSubmix.IsValid()
			? Out.TargetSubmix->GetName() : TEXT("Master");
		StopOutput(Out);
		Outputs.RemoveAt(Index);
		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Receive output removed — '%s' -> Submix '%s'"),
			*ChannelName, *SubmixName);
	}

	/** Stop all outputs and recreate them on the new device.
	 *  Pass INDEX_NONE to deactivate without recreating. */
	void SwitchToDevice(Audio::FDeviceId NewDeviceId)
	{
		FScopeLock Lock(&OutputsLock);
		DeactivateLocked();
		ActiveDeviceId = NewDeviceId;
		if (NewDeviceId == INDEX_NONE)
		{
			return;
		}

		FAudioDeviceManager* ADM = FAudioDeviceManager::Get();
		if (!ADM) return;
		FAudioDeviceHandle NewHandle = ADM->GetAudioDevice(NewDeviceId);
		FAudioDevice* NewDevice = NewHandle.GetAudioDevice();
		if (!NewDevice) return;

		for (FOutput& Out : Outputs)
		{
			CreateProceduralSound(Out, NewDevice);
		}
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
	/** Stop all playback without removing logical outputs. Must be called under OutputsLock. */
	void DeactivateLocked()
	{
		FAudioDeviceManager* ADM = FAudioDeviceManager::Get();
		FAudioDevice* Device = nullptr;
		if (ADM && ActiveDeviceId != INDEX_NONE)
		{
			FAudioDeviceHandle Handle = ADM->GetAudioDevice(ActiveDeviceId);
			Device = Handle.GetAudioDevice();
		}

		for (FOutput& Out : Outputs)
		{
			if (Device && Out.ProceduralSound.IsValid())
			{
				Device->StopSoundsUsingResource(Out.ProceduralSound.Get());
			}
			Out.ProceduralSound.Reset();
		}
		ActiveDeviceId = INDEX_NONE;
	}

	/** Create a ProceduralSound + FActiveSound for one output on the given device.
	 *
	 *  NOTE — PlatformAudioHeadroom compensation:
	 *  UE applies a per-platform headroom scalar to every source's volume
	 *  (e.g. Mac = -6 dB, Windows = -3 dB) to prevent clipping when many game
	 *  sounds are summed.  Link4UE acts as an audio pass-through, not a game
	 *  sound generator, so this headroom would introduce unwanted gain loss.
	 *  We read the same ini value the engine uses (Audio.PlatformHeadroomDB)
	 *  and set VolumeMultiplier to its reciprocal to cancel the reduction.
	 *
	 *  Caveat: if SetPlatformAudioHeadroom() is called at runtime to change
	 *  the value after the ProceduralSound has been created, the compensation
	 *  here will be stale and a gain mismatch will occur.  This is acceptable
	 *  because runtime headroom changes are extremely rare in practice. */
	void CreateProceduralSound(FOutput& Out, FAudioDevice* Device)
	{
		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(
			GetTransientPackage());
		const int32 Rate = Device->GetSampleRate();
		Wave->SetSampleRate(Rate);
		Wave->NumChannels = 2;
		Wave->Duration = INDEFINITELY_LOOPING_DURATION;
		Wave->SoundGroup = SOUNDGROUP_Default;
		Wave->bLooping = true;

		FActiveSound NewActiveSound;
		NewActiveSound.SetSound(Wave);
		NewActiveSound.SetWorld(nullptr);
		NewActiveSound.bAllowSpatialization = false;
		NewActiveSound.bIsUISound = true;
		NewActiveSound.bLocationDefined = false;
		NewActiveSound.bIgnoreForFlushing = true;

		// Compensate PlatformAudioHeadroom (see note above)
		float HeadroomLinear = 1.0f;
		float HeadroomDB = 0.0f;
		if (GConfig->GetFloat(TEXT("Audio"), TEXT("PlatformHeadroomDB"), HeadroomDB, GEngineIni))
		{
			HeadroomLinear = FMath::Pow(10.0f, HeadroomDB / 20.0f);
		}
		if (HeadroomLinear > SMALL_NUMBER)
		{
			NewActiveSound.SetVolume(1.0f / HeadroomLinear);
		}

		USoundSubmix* TargetSubmix = Out.TargetSubmix.Get();
		if (TargetSubmix)
		{
			FSoundSubmixSendInfo SubmixSend;
			SubmixSend.SoundSubmix = TargetSubmix;
			SubmixSend.SendLevel = 1.0f;
			NewActiveSound.SetSubmixSend(SubmixSend);
		}

		Device->AddNewActiveSound(NewActiveSound);
		Out.ProceduralSound.Reset(Wave);

		DeviceSampleRate.store(Rate, std::memory_order_relaxed);

		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE CreateProceduralSound: rate=%d, PlatformHeadroomDB=%.1f, VolumeCompensation=%.4f"),
			Rate, HeadroomDB, (HeadroomLinear > SMALL_NUMBER) ? 1.0f / HeadroomLinear : 1.0f);
	}

	/** Stop and release a single output's playback. Must be called under OutputsLock. */
	void StopOutput(FOutput& Out)
	{
		if (Out.ProceduralSound.IsValid() && ActiveDeviceId != INDEX_NONE)
		{
			if (FAudioDeviceManager* ADM = FAudioDeviceManager::Get())
			{
				ADM->StopSoundsUsingResource(Out.ProceduralSound.Get());
			}
		}
		Out.ProceduralSound.Reset();
	}

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
			AudioData = reinterpret_cast<const uint8*>(Handle.samples);
			AudioBytes = SrcFrames * SrcChannels * sizeof(int16);
		}
		else
		{
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

		// Mono→stereo: duplicate each sample to L and R (unity gain, DAW convention).
		// Wave is always 2ch; queuing 1ch data into a 2ch Wave breaks the mixer.
		if (SrcChannels == 1)
		{
			const int16* MonoData = reinterpret_cast<const int16*>(AudioData);
			const int32 MonoSamples = AudioBytes / sizeof(int16);
			MonoToStereoBuffer.SetNumUninitialized(MonoSamples * 2, EAllowShrinking::No);
			for (int32 i = 0; i < MonoSamples; ++i)
			{
				MonoToStereoBuffer[i * 2]     = MonoData[i];
				MonoToStereoBuffer[i * 2 + 1] = MonoData[i];
			}
			AudioData = reinterpret_cast<const uint8*>(MonoToStereoBuffer.GetData());
			AudioBytes = MonoSamples * 2 * sizeof(int16);
		}

		// Push to all outputs (single device)
		FScopeLock Lock(&OutputsLock);
		for (FOutput& Out : Outputs)
		{
			if (Out.ProceduralSound.IsValid())
			{
				// Overrun protection: if queued data exceeds the threshold,
				// reset the buffer to prevent unbounded latency accumulation.
				// This causes a click but is preferable to growing delay.
				if (Out.ProceduralSound->GetAvailableAudioByteCount() > OverrunThresholdBytes)
				{
					Out.ProceduralSound->ResetAudio();
					UE_LOG(LogLink4UE, Warning,
						TEXT("Link4UE [RECV] '%s': overrun detected (%d bytes queued, threshold=%d) — reset"),
						*ChannelName, Out.ProceduralSound->GetAvailableAudioByteCount(), OverrunThresholdBytes);
				}
				Out.ProceduralSound->QueueAudio(AudioData, AudioBytes);
			}
		}
	}

	Audio::FDeviceId ActiveDeviceId = INDEX_NONE;
	TArray<FOutput> Outputs;
	FCriticalSection OutputsLock;
	FString ChannelIdHex;
	std::atomic<int32> DeviceSampleRate;
	FString ChannelName;
	ableton::LinkAudioSource Source; // Must be last — destructor stops callback first

	TArray<int16> ResampleBuffer;
	TArray<int16> MonoToStereoBuffer;

	// Overrun threshold: 3 callback buffers worth of stereo int16 data.
	// Mac CoreAudio = 1024 frames; other platforms may be smaller but 1024 is a safe upper bound.
	// 1024 frames × 2 ch × 2 bytes × 3 = 12288 bytes
	static constexpr int32 OverrunThresholdBytes = 1024 * 2 * sizeof(int16) * 3;

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

	void TearDownMasterSend(Audio::FDeviceId OnDeviceId)
	{
		FAudioDevice* AudioDevice = GetDeviceById(OnDeviceId);
		if (AudioDevice && MasterSend.Bridge.IsValid() && MasterSend.Submix.IsValid())
		{
			AudioDevice->UnregisterSubmixBufferListener(
				MasterSend.Bridge.ToSharedRef(), *MasterSend.Submix.Get());
		}
		MasterSend.Bridge.Reset();
		MasterSend.Submix.Reset();
	}

	void TearDownUserSends(Audio::FDeviceId OnDeviceId)
	{
		FAudioDevice* AudioDevice = GetDeviceById(OnDeviceId);
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

	void TearDownAllSends(Audio::FDeviceId OnDeviceId)
	{
		TearDownMasterSend(OnDeviceId);
		TearDownUserSends(OnDeviceId);
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

	LinkInstance = new FLinkInstance(DefaultTempo,
		TCHAR_TO_UTF8(*PeerName));

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
	ApplySettings();

	// Capture initial channel snapshot for diff logging
	LinkInstance->LogChannelDiff();

	// Listen for AudioDevice lifecycle events
	AudioDeviceCreatedHandle = FAudioDeviceManagerDelegates::OnAudioDeviceCreated.AddUObject(
		this, &ULink4UESubsystem::OnAudioDeviceCreated);
	AudioDeviceDestroyedHandle = FAudioDeviceManagerDelegates::OnAudioDeviceDestroyed.AddUObject(
		this, &ULink4UESubsystem::OnAudioDeviceDestroyed);

#if WITH_EDITOR
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule)
	{
		SettingsModule->RegisterSettings(
			TEXT("Project"), TEXT("Plugins"), TEXT("Link4UE"),
			NSLOCTEXT("Link4UE", "SettingsSection", "Link4UE"),
			NSLOCTEXT("Link4UE", "SettingsDescription", "Ableton Link synchronization settings."),
			this);
	}
#endif
}

void ULink4UESubsystem::Deinitialize()
{
	FAudioDeviceManagerDelegates::OnAudioDeviceCreated.Remove(AudioDeviceCreatedHandle);
	FAudioDeviceManagerDelegates::OnAudioDeviceDestroyed.Remove(AudioDeviceDestroyedHandle);

#if WITH_EDITOR
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule)
	{
		SettingsModule->UnregisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("Link4UE"));
	}
#endif
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);

	if (LinkInstance)
	{
		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Shutting down (sends=%d, receives=%d)"),
			LinkInstance->ActiveSends.Num(), LinkInstance->ActiveReceives.Num());

		LinkInstance->TearDownReceives();
		LinkInstance->TearDownAllSends(CurrentActiveDeviceId);
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

	UE_LOG(LogLink4UE, Log,
		TEXT("Link4UE: Audio device created (id=%u)"), DeviceId);

	// Rebuild deferred send routes
	if (bSendRoutesPending)
	{
		RebuildAudioSends();
	}

	// Rebuild deferred receive routes
	if (bReceiveRoutesPending)
	{
		RebuildAudioReceives();
	}

	// A new device may become active — check on next Tick
	bNeedsAudioRecreation = true;
}

void ULink4UESubsystem::OnAudioDeviceDestroyed(Audio::FDeviceId DeviceId)
{
	UE_LOG(LogLink4UE, Log,
		TEXT("Link4UE: Audio device destroyed (id=%u, activeSends=%d, activeReceives=%d)"),
		DeviceId,
		LinkInstance ? LinkInstance->ActiveSends.Num() : 0,
		LinkInstance ? LinkInstance->ActiveReceives.Num() : 0);

	if (!LinkInstance)
	{
		return;
	}

	// If the destroyed device is our current device, deactivate receive outputs
	// (device is still alive at broadcast time, so StopSoundsUsingResource is safe)
	if (DeviceId == CurrentActiveDeviceId)
	{
		for (auto& Recv : LinkInstance->ActiveReceives)
		{
			Recv->SwitchToDevice(INDEX_NONE);
		}
	}

	// Trigger recreation on the (new) active device on next Tick
	bNeedsAudioRecreation = true;
}

// ---------------------------------------------------------------------------
// Settings hot-reload
// ---------------------------------------------------------------------------

void ULink4UESubsystem::ApplySettings()
{
	if (!LinkInstance)
	{
		return;
	}

	// enable() must come before enableLinkAudio() — the SDK requires
	// mEnabled == true for Link Audio to activate.
	LinkInstance->Link.enable(bAutoConnect);
	LinkInstance->Link.enableStartStopSync(bStartStopSync);
	LinkInstance->Link.setPeerName(TCHAR_TO_UTF8(*PeerName));
	LinkInstance->Link.enableLinkAudio(bEnableLinkAudio);

	const double Q = Link4UEQuantumToBeats(DefaultQuantum);
	Quantum.store(Q, std::memory_order_relaxed);

	SetTempo(DefaultTempo);

	bIsRebuilding = true;
	RebuildAudioSends();
	RebuildAudioReceives();
	bIsRebuilding = false;
	bChannelsDirty.store(false, std::memory_order_relaxed);

	UE_LOG(LogLink4UE, Log,
		TEXT("Link4UE: Settings applied — tempo=%.1f, connect=%s, audio=%s, peer='%s'"),
		DefaultTempo,
		bAutoConnect ? TEXT("on") : TEXT("off"),
		bEnableLinkAudio ? TEXT("on") : TEXT("off"),
		*PeerName);
}

void ULink4UESubsystem::RebuildAudioSends()
{
	if (!LinkInstance)
	{
		return;
	}

	if (!bEnableLinkAudio)
	{
		LinkInstance->TearDownAllSends(CurrentActiveDeviceId);
		bSendRoutesPending = false;
		return;
	}

	FAudioDevice* AudioDevice = GetActiveDevice();
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
	for (const FLink4UEAudioSend& SendDef : AudioSends)
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

void ULink4UESubsystem::RebuildAudioReceives()
{
	if (!LinkInstance)
	{
		return;
	}

	if (!bEnableLinkAudio)
	{
		LinkInstance->TearDownReceives();
		bReceiveRoutesPending = false;
		return;
	}

	FAudioDevice* ActiveDevice = GetActiveDevice();
	if (!ActiveDevice)
	{
		bReceiveRoutesPending = true;
		UE_LOG(LogLink4UE, Warning,
			TEXT("Link4UE: No audio device available, receive routes deferred"));
		return;
	}

	const Audio::FDeviceId ActiveId = ActiveDevice->DeviceID;
	CurrentActiveDeviceId = ActiveId;

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

	const int32 DeviceSampleRate = ActiveDevice->GetSampleRate();

	// --- Build desired state keyed by ChannelId ---
	struct FDesiredOutput { USoundSubmix* Submix; };
	struct FDesiredChannel
	{
		ableton::ChannelId Id;
		FString Name;
		TArray<FDesiredOutput> Outputs;
	};
	TMap<FString, FDesiredChannel> DesiredMap; // Key = ChannelIdHex

	for (const FLink4UEAudioReceive& RecvDef : AudioReceives)
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
				Bridge->RemoveOutput(Ai);
			}
		}

		for (int32 Di : UnmatchedDesired)
		{
			Bridge->AddOutput(DC.Outputs[Di].Submix, ActiveDevice);
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
	bool bAnyChanged = false;

	for (FLink4UEAudioReceive& Recv : AudioReceives)
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
		SaveConfig();
	}
}

void ULink4UESubsystem::NotifyPropertyChanged(FName PropertyName,
	EPropertyChangeType::Type ChangeType)
{
#if WITH_EDITOR
	FProperty* Prop = GetClass()->FindPropertyByName(PropertyName);
	if (Prop)
	{
		FPropertyChangedEvent Event(Prop, ChangeType);
		PostEditChangeProperty(Event);
	}
#else
	// Shipping build — apply side effects directly (no PostEditChangeProperty override)
	if (!LinkInstance)
	{
		return;
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bEnableLinkAudio))
	{
		LinkInstance->Link.enableLinkAudio(bEnableLinkAudio);
		bIsRebuilding = true;
		RebuildAudioSends();
		RebuildAudioReceives();
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioSends))
	{
		bIsRebuilding = true;
		RebuildAudioSends();
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioReceives))
	{
		bIsRebuilding = true;
		RebuildAudioReceives();
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bStartStopSync))
	{
		LinkInstance->Link.enableStartStopSync(bStartStopSync);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, PeerName))
	{
		LinkInstance->Link.setPeerName(TCHAR_TO_UTF8(*PeerName));
	}
#endif
}

#if WITH_EDITOR
void ULink4UESubsystem::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (!LinkInstance)
	{
		return;
	}

	const FName PropName = PropertyChangedEvent.GetMemberPropertyName();

	if (PropName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioSends))
	{
		// Ensure all sends have a RouteId (editor UI creates entries without one)
		for (FLink4UEAudioSend& Send : AudioSends)
		{
			if (!Send.RouteId.IsValid())
			{
				Send.RouteId = FGuid::NewGuid();
			}
		}
		bIsRebuilding = true;
		RebuildAudioSends();
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
		return;
	}
	if (PropName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioReceives))
	{
		bIsRebuilding = true;
		RebuildAudioReceives();
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
		return;
	}

	// bEnableLinkAudio toggles audio routes on/off — rebuild needed
	if (PropName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bEnableLinkAudio))
	{
		LinkInstance->Link.enableLinkAudio(bEnableLinkAudio);
		bIsRebuilding = true;
		RebuildAudioSends();
		RebuildAudioReceives();
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
		return;
	}

	// Per-property dispatch — only apply the side effect for the changed property
	if (PropName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bStartStopSync))
	{
		LinkInstance->Link.enableStartStopSync(bStartStopSync);
	}
	else if (PropName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, PeerName))
	{
		LinkInstance->Link.setPeerName(TCHAR_TO_UTF8(*PeerName));
	}
	else if (PropName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bAutoConnect))
	{
		LinkInstance->Link.enable(bAutoConnect);
	}
	else if (PropName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, DefaultQuantum))
	{
		const double Q = Link4UEQuantumToBeats(DefaultQuantum);
		Quantum.store(Q, std::memory_order_relaxed);
	}
	else if (PropName == GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, DefaultTempo))
	{
		SetTempo(DefaultTempo);
	}
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
			RebuildAudioReceives();
			bIsRebuilding = false;
			bChannelsDirty.store(false, std::memory_order_relaxed);
		}
		OnChannelsChanged.Broadcast();
	}

	// Retry deferred audio routes once AudioDevice becomes available
	if ((bSendRoutesPending || bReceiveRoutesPending) && GetActiveDevice())
	{
		bIsRebuilding = true;
		if (bSendRoutesPending)
		{
			UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Audio device ready, rebuilding deferred send routes"));
			RebuildAudioSends();
		}
		if (bReceiveRoutesPending)
		{
			UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Rebuilding deferred receive routes"));
			RebuildAudioReceives();
		}
		bIsRebuilding = false;
		bChannelsDirty.store(false, std::memory_order_relaxed);
	}

	// Active device monitoring — recreate audio when device changes or after destruction
	if (bEnableLinkAudio)
	{
		if (FAudioDeviceManager* ADM = FAudioDeviceManager::Get())
		{
			FAudioDeviceHandle ActiveHandle = ADM->GetActiveAudioDevice();
			const Audio::FDeviceId ActiveId = ActiveHandle.IsValid()
				? ActiveHandle.GetAudioDevice()->DeviceID
				: static_cast<Audio::FDeviceId>(INDEX_NONE);

			if (ActiveId != CurrentActiveDeviceId || bNeedsAudioRecreation)
			{
				if (ActiveId != INDEX_NONE)
				{
					RecreateAudioOnDevice(ActiveId);
				}
			}
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
// Audio device transition — recreate all send/receive routes on a new device
// ---------------------------------------------------------------------------

void ULink4UESubsystem::RecreateAudioOnDevice(Audio::FDeviceId DeviceId)
{
	if (!LinkInstance || !bEnableLinkAudio)
	{
		return;
	}

	FAudioDeviceManager* ADM = FAudioDeviceManager::Get();
	if (!ADM)
	{
		return;
	}

	FAudioDeviceHandle NewHandle = ADM->GetAudioDevice(DeviceId);
	FAudioDevice* NewDevice = NewHandle.GetAudioDevice();
	if (!NewDevice)
	{
		return;
	}

	const Audio::FDeviceId OldId = CurrentActiveDeviceId;

	// --- Receive: switch all bridges to the new device ---
	for (auto& Recv : LinkInstance->ActiveReceives)
	{
		Recv->SwitchToDevice(DeviceId);
	}

	// --- Send: re-register listeners on the new device ---

	FAudioDevice* OldDevice = GetDeviceById(OldId);

	auto MigrateSend = [&](FLinkInstance::FActiveSend& Send)
	{
		if (!Send.Bridge.IsValid() || !Send.Submix.IsValid()) return;
		if (OldDevice)
		{
			OldDevice->UnregisterSubmixBufferListener(
				Send.Bridge.ToSharedRef(), *Send.Submix.Get());
		}
		NewDevice->RegisterSubmixBufferListener(
			Send.Bridge.ToSharedRef(), *Send.Submix.Get());
	};

	MigrateSend(LinkInstance->MasterSend);
	for (auto& Send : LinkInstance->ActiveSends)
	{
		MigrateSend(Send);
	}

	CurrentActiveDeviceId = DeviceId;
	bNeedsAudioRecreation = false;

	UE_LOG(LogLink4UE, Log,
		TEXT("Link4UE: Audio routes migrated (device %u -> %u, receives=%d, sends=%d)"),
		OldId, DeviceId,
		LinkInstance->ActiveReceives.Num(),
		LinkInstance->ActiveSends.Num() + (LinkInstance->MasterSend.Bridge.IsValid() ? 1 : 0));
}

// ---------------------------------------------------------------------------
// Enable / Disable
// ---------------------------------------------------------------------------

void ULink4UESubsystem::EnableLink()
{
	if (!LinkInstance) return;
	bAutoConnect = true;
	NotifyPropertyChanged(GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bAutoConnect));
	UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Link enabled"));
}

void ULink4UESubsystem::DisableLink()
{
	if (!LinkInstance) return;
	bAutoConnect = false;
	NotifyPropertyChanged(GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bAutoConnect));
	UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Link disabled"));
}

bool ULink4UESubsystem::IsLinkEnabled() const
{
	return LinkInstance && LinkInstance->Link.isEnabled();
}

void ULink4UESubsystem::EnableStartStopSync(bool bEnable)
{
	if (!LinkInstance) return;
	bStartStopSync = bEnable;
	NotifyPropertyChanged(GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bStartStopSync));
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
	if (!LinkInstance) return;
	bEnableLinkAudio = bEnable;
	NotifyPropertyChanged(GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, bEnableLinkAudio));
	UE_LOG(LogLink4UE, Log, TEXT("Link4UE: Link Audio %s"), bEnable ? TEXT("enabled") : TEXT("disabled"));
}

bool ULink4UESubsystem::IsLinkAudioEnabled() const
{
	return LinkInstance && LinkInstance->Link.isLinkAudioEnabled();
}

void ULink4UESubsystem::SetPeerName(const FString& InPeerName)
{
	if (!LinkInstance) return;
	PeerName = InPeerName;
	NotifyPropertyChanged(GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, PeerName));
}

// ---------------------------------------------------------------------------
// Audio Send Mutators
// ---------------------------------------------------------------------------

FGuid ULink4UESubsystem::AddAudioSend(USoundSubmix* Submix, const FString& ChannelNamePrefix)
{
	FLink4UEAudioSend Send;
	Send.RouteId = FGuid::NewGuid();
	Send.Submix = Submix;
	Send.ChannelNamePrefix = ChannelNamePrefix;

	AudioSends.Add(MoveTemp(Send));
	NotifyPropertyChanged(
		GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioSends),
		EPropertyChangeType::ArrayAdd);

	return AudioSends.Last().RouteId;
}

bool ULink4UESubsystem::RemoveAudioSend(const FGuid& RouteId)
{
	if (!RouteId.IsValid())
	{
		return false;
	}

	const int32 Idx = AudioSends.IndexOfByPredicate(
		[&RouteId](const FLink4UEAudioSend& S) { return S.RouteId == RouteId; });

	if (Idx == INDEX_NONE)
	{
		return false;
	}

	AudioSends.RemoveAt(Idx);
	NotifyPropertyChanged(
		GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioSends),
		EPropertyChangeType::ArrayRemove);

	return true;
}

void ULink4UESubsystem::ClearAudioSends()
{
	if (AudioSends.IsEmpty())
	{
		return;
	}

	AudioSends.Empty();
	NotifyPropertyChanged(
		GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioSends),
		EPropertyChangeType::ArrayClear);
}

// ---------------------------------------------------------------------------
// Audio Receive Mutators
// ---------------------------------------------------------------------------

bool ULink4UESubsystem::AddAudioReceive(const FString& ChannelId, USoundSubmix* Submix)
{
	if (ChannelId.IsEmpty())
	{
		return false;
	}

	// Resolve ChannelName from live session
	FString ChannelName;
	if (LinkInstance)
	{
		for (const auto& Ch : LinkInstance->Link.channels())
		{
			if (NodeIdToHex(Ch.id) == ChannelId)
			{
				ChannelName = UTF8_TO_TCHAR(Ch.name.c_str());
				break;
			}
		}
	}

	FLink4UEAudioReceive Recv;
	Recv.ChannelId = ChannelId;
	Recv.ChannelName = ChannelName;
	Recv.Submix = Submix;

	AudioReceives.Add(MoveTemp(Recv));
	NotifyPropertyChanged(
		GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioReceives),
		EPropertyChangeType::ArrayAdd);

	return true;
}

bool ULink4UESubsystem::RemoveAudioReceive(const FString& ChannelId)
{
	if (ChannelId.IsEmpty())
	{
		return false;
	}

	const int32 CountBefore = AudioReceives.Num();
	AudioReceives.RemoveAll(
		[&ChannelId](const FLink4UEAudioReceive& R) { return R.ChannelId == ChannelId; });

	if (AudioReceives.Num() == CountBefore)
	{
		return false;
	}

	NotifyPropertyChanged(
		GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioReceives),
		EPropertyChangeType::ArrayRemove);

	return true;
}

void ULink4UESubsystem::ClearAudioReceives()
{
	if (AudioReceives.IsEmpty())
	{
		return;
	}

	AudioReceives.Empty();
	NotifyPropertyChanged(
		GET_MEMBER_NAME_CHECKED(ULink4UESubsystem, AudioReceives),
		EPropertyChangeType::ArrayClear);
}

// ---------------------------------------------------------------------------
// Channel Query
// ---------------------------------------------------------------------------

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
