// Copyright YUKITAKA. All Rights Reserved.

#include "Link4UESubsystem.h"
#include "Link4UEProceduralSound.h"
#if WITH_EDITOR
#include "ISettingsModule.h"
#endif
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "ISubmixBufferListener.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundGenerator.h"
#include "DSP/Dsp.h"
#include "ActiveSound.h"
#include "ableton/LinkAudio.hpp"
#include "ableton/util/FloatIntConversion.hpp"

namespace
{
	constexpr size_t kDefaultMaxSamples = 8192;
	constexpr float kInt16ToFloat = 1.0f / 32768.0f;

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

// Link4UE.LatencyLog — 0: off, 1: periodic summary (1 sec), 2: every callback
static TAutoConsoleVariable<int32> CVarLatencyLog(
	TEXT("Link4UE.LatencyLog"),
	0,
	TEXT("Link Audio receive latency logging level.\n")
	TEXT("  0 = Off\n")
	TEXT("  1 = Periodic summary (avg/max per second)\n")
	TEXT("  2 = Every callback (high volume debug)"),
	ECVF_Default);

// Link4UE.JitterBuffer — -1: adaptive (default), 0: minimum latency, N: fixed N ms
static TAutoConsoleVariable<int32> CVarJitterBuffer(
	TEXT("Link4UE.JitterBuffer"),
	-1,
	TEXT("Jitter buffer target for Link Audio receive.\n")
	TEXT("  -1 = Adaptive (underrun-driven, default)\n")
	TEXT("   0 = Minimum latency (1 render cycle cushion)\n")
	TEXT("   N = Fixed target of N milliseconds"),
	ECVF_Default);

// ---------------------------------------------------------------------------
// FLink4UESoundGenerator — ISoundGenerator that pulls from a lock-free ring
//   buffer fed by LinkAudio SDK callbacks.
//
//   Consumer-side buffer management:
//   - Producer (SDK thread) only pushes data, never flushes.
//   - Consumer (mixer thread) trims excess to maintain target level.
//   - Adaptive target: increases fast on underrun, decreases slowly when stable.
//   - All parameters derived from runtime values (no hardcoded constants).
// ---------------------------------------------------------------------------

class FLink4UESoundGenerator : public ISoundGenerator
{
public:
	FLink4UESoundGenerator(float InSampleRate, int32 InRenderCycleFrames, int32 InNumChannels)
		: SampleRate(InSampleRate)
		, NumChannels(InNumChannels)
		, RenderCycleSamples(InRenderCycleFrames * InNumChannels)
		, RingBuffer(static_cast<uint32>(InSampleRate * InNumChannels))  // 1 second capacity
		, TargetSamples(InRenderCycleFrames * InNumChannels * 2)
		, MinTargetSamples(InRenderCycleFrames * InNumChannels)
		, MaxTargetSamples(static_cast<int32>(InSampleRate * InNumChannels / 2))  // 500ms
		, StableThreshold(static_cast<int32>((InSampleRate / InRenderCycleFrames) * 5))  // ~5 sec
	{
	}

	//~ ISoundGenerator interface
	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
	{
		// --- CVar check: react to dynamic changes ---
		const int32 CVarVal = CVarJitterBuffer.GetValueOnAnyThread();
		if (CVarVal != LastCVarValue)
		{
			LastCVarValue = CVarVal;
			if (CVarVal == 0)
			{
				TargetSamples = MinTargetSamples;
			}
			else if (CVarVal > 0)
			{
				TargetSamples = FMath::Clamp(
					static_cast<int32>(CVarVal * SampleRate * NumChannels / 1000.0f),
					MinTargetSamples, MaxTargetSamples);
			}
			// CVarVal == -1: adaptive mode, don't force-set target
			StableCount = 0;
		}
		const bool bAdaptive = (CVarVal < 0);

		// --- Trim excess: keep TargetSamples + NumSamples, discard oldest ---
		// Pop(uint32) advances ReadCounter without copying data.
		const int32 Available = static_cast<int32>(RingBuffer.Num());
		const int32 Excess = Available - TargetSamples - NumSamples;
		if (Excess > 0)
		{
			RingBuffer.Pop(static_cast<uint32>(Excess));
			TrimCount.fetch_add(1, std::memory_order_relaxed);
		}

		// --- Pop requested samples ---
		const int32 Popped = RingBuffer.Pop(OutAudio, NumSamples);

		// --- Underrun: zero-fill remainder ---
		bool bUnderrun = false;
		if (Popped < NumSamples)
		{
			FMemory::Memzero(OutAudio + Popped, (NumSamples - Popped) * sizeof(float));
			bUnderrun = true;
		}

		// --- Adaptive target update (only in auto mode, after first data received) ---
		if (bAdaptive && bEverReceived.load(std::memory_order_relaxed))
		{
			if (bUnderrun)
			{
				// Fast increase: deficit + 1 render cycle margin,
				// capped at 2 render cycles to avoid runaway growth
				// under periodic jitter.
				const int32 Deficit = NumSamples - Popped;
				const int32 Increase = FMath::Min(Deficit + NumSamples, RenderCycleSamples * 2);
				TargetSamples += Increase;
				TargetSamples = FMath::Min(TargetSamples, MaxTargetSamples);
				StableCount = 0;
			}
			else if (++StableCount > StableThreshold)
			{
				// Slow decrease: 1/4 render cycle per stable period (min 1 sample)
				TargetSamples -= FMath::Max(NumSamples / 4, 1);
				TargetSamples = FMath::Max(TargetSamples, MinTargetSamples);
				StableCount = 0;
			}
		}

		return NumSamples;
	}

	virtual int32 GetDesiredNumSamplesToRenderPerCallback() const override
	{
		return RenderCycleSamples;
	}

	/** Push float audio data from the SDK callback thread. Producer only pushes. */
	void PushAudio(const float* Data, int32 NumSamples)
	{
		bEverReceived.store(true, std::memory_order_relaxed);
		RingBuffer.Push(Data, NumSamples);
	}

	int32 GetAndResetTrimCount()
	{
		return TrimCount.exchange(0, std::memory_order_relaxed);
	}

private:
	float SampleRate;
	int32 NumChannels;
	int32 RenderCycleSamples;
	Audio::TCircularAudioBuffer<float> RingBuffer;

	// Adaptive state (audio thread only — no atomics needed)
	int32 TargetSamples;
	int32 MinTargetSamples;
	int32 MaxTargetSamples;
	int32 StableThreshold;
	int32 StableCount = 0;
	int32 LastCVarValue = -1;

	// Cross-thread state
	std::atomic<bool> bEverReceived{false};     // Producer → Consumer
	std::atomic<int32> TrimCount{0};            // Consumer → GameThread (monitoring)
};

// ---------------------------------------------------------------------------
// ULink4UEProceduralSound implementation
// ---------------------------------------------------------------------------

ISoundGeneratorPtr ULink4UEProceduralSound::CreateSoundGenerator(
	const FSoundGeneratorInitParams& InParams)
{
	Generator = MakeShared<FLink4UESoundGenerator>(
		InParams.SampleRate,
		InParams.AudioMixerNumOutputFrames,
		InParams.NumChannels);
	return Generator;
}

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
//   via ULink4UEProceduralSound + ISoundGenerator (pull model)
//
//   Active-device-only design: all outputs play on a single device at a time.
//   SwitchToDevice() stops the old device's sounds and recreates on the new one.
// ---------------------------------------------------------------------------

class FLink4UEReceiveBridge
{
public:
	/** Logical output (one per TargetSubmix + ChannelFormat pair). */
	struct FOutput
	{
		TWeakObjectPtr<USoundSubmix> TargetSubmix; // null = Master, used for diff matching
		TStrongObjectPtr<ULink4UEProceduralSound> ProceduralSound;
		ELink4UEChannelFormat ChannelFormat = ELink4UEChannelFormat::Stereo;
	};

	FLink4UEReceiveBridge(ableton::LinkAudio& InLink,
		const ableton::ChannelId& InChannelId,
		int32 InDeviceSampleRate,
		const FString& InChannelName,
		const std::atomic<double>& InQuantumRef)
		: LinkRef(InLink)
		, QuantumRef(InQuantumRef)
		, ChannelIdHex(NodeIdToHex(InChannelId))
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
		//    → SDK callback stops, no more PushAudio calls
		// 2. Stop FActiveSounds on the active device
		// 3. TStrongObjectPtr releases ULink4UEProceduralSound
		FScopeLock Lock(&OutputsLock);
		DeactivateLocked();
	}

	/** Add a logical output and create playback on the given device. */
	void AddOutput(USoundSubmix* InTargetSubmix, FAudioDevice* Device,
		ELink4UEChannelFormat InFormat = ELink4UEChannelFormat::Stereo)
	{
		FScopeLock Lock(&OutputsLock);
		FOutput& Out = Outputs.AddDefaulted_GetRef();
		Out.TargetSubmix = InTargetSubmix;
		Out.ChannelFormat = InFormat;

		if (Device)
		{
			CreateProceduralSound(Out, Device);
			ActiveDeviceId = Device->DeviceID;
		}

		UE_LOG(LogLink4UE, Log,
			TEXT("Link4UE: Receive output added — '%s' -> Submix '%s' (%s, device=%u)"),
			*ChannelName,
			InTargetSubmix ? *InTargetSubmix->GetName() : TEXT("Master"),
			InFormat == ELink4UEChannelFormat::Mono ? TEXT("mono") : TEXT("stereo"),
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

		// Discard the first Source callback after switch — it contains data
		// that accumulated while OutputsLock was held during the transition.
		// Without this, that stale data becomes initial playback latency.
		bFlushNextCallback.store(true, std::memory_order_release);
	}

	int32 GetNumOutputs() const { return Outputs.Num(); }
	USoundSubmix* GetOutputSubmix(int32 Index) const
	{
		return Outputs.IsValidIndex(Index) ? Outputs[Index].TargetSubmix.Get() : nullptr;
	}
	ELink4UEChannelFormat GetOutputChannelFormat(int32 Index) const
	{
		return Outputs.IsValidIndex(Index) ? Outputs[Index].ChannelFormat : ELink4UEChannelFormat::Stereo;
	}

	const FString& GetChannelIdHex() const { return ChannelIdHex; }
	const FString& GetChannelName() const { return ChannelName; }
	void SetChannelName(const FString& NewName) { ChannelName = NewName; }

	/** Collect and reset trim counts from all active generators. Returns total trims. */
	int32 DrainTrimCount()
	{
		int32 Total = 0;
		FScopeLock Lock(&OutputsLock);
		for (FOutput& Out : Outputs)
		{
			if (Out.ProceduralSound.IsValid())
			{
				if (FLink4UESoundGenerator* Gen = Out.ProceduralSound->GetGenerator())
				{
					Total += Gen->GetAndResetTrimCount();
				}
			}
		}
		return Total;
	}

	/** Drain latency stats accumulated since last call. Returns false if no samples. */
	bool DrainLatencyStats(double& OutAvgMs, double& OutMaxMs, int32& OutCount)
	{
		return LatencyStats.Drain(OutAvgMs, OutMaxMs, OutCount);
	}

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
		ULink4UEProceduralSound* Wave = NewObject<ULink4UEProceduralSound>(
			GetTransientPackage());
		const int32 Rate = Device->GetSampleRate();
		Wave->SetSampleRate(Rate);
		Wave->NumChannels = (Out.ChannelFormat == ELink4UEChannelFormat::Mono) ? 1 : 2;
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
		// After a device switch, discard the first callback to avoid
		// queuing stale data that accumulated during the transition.
		if (bFlushNextCallback.exchange(false, std::memory_order_acquire))
		{
			return;
		}

		const int32 SrcRate = static_cast<int32>(Handle.info.sampleRate);
		const int32 SrcFrames = static_cast<int32>(Handle.info.numFrames);
		const int32 SrcChannels = static_cast<int32>(Handle.info.numChannels);

		if (SrcFrames == 0 || SrcChannels == 0)
		{
			return;
		}

		// --- Latency measurement via Link beat clock ---
		const int32 LatencyLogLevel = CVarLatencyLog.GetValueOnAnyThread();
		if (LatencyLogLevel > 0)
		{
			const double Q = QuantumRef.load(std::memory_order_relaxed);
			const auto Now = LinkRef.clock().micros();
			const auto State = LinkRef.captureAudioSessionState();
			const double NowBeats = State.beatAtTime(Now, Q);
			const double Tempo = Handle.info.tempo;

			const auto MappedBeats = Handle.info.beginBeats(State, Q);
			if (Tempo > 0.0 && MappedBeats.has_value())
			{
				const double LatencyBeats = NowBeats - MappedBeats.value();
				const double LatencyMs = (LatencyBeats / Tempo) * 60000.0;

				// Level 1: accumulate for GameThread summary
				LatencyStats.Record(LatencyMs);

				// Level 2: per-callback log (high volume)
				if (LatencyLogLevel >= 2)
				{
					UE_LOG(LogLink4UE, Log,
						TEXT("Link4UE [RECV] '%s': SDK latency=%.1fms (%.3f beats, %.1f BPM, count=%llu, frames=%d)"),
						*ChannelName, LatencyMs, LatencyBeats, Tempo,
						Handle.info.count, SrcFrames);
				}
			}
		}

		// --- Resample (int16) ---
		const int16* Int16Data;
		int32 Frames;

		const int32 DstRate = DeviceSampleRate.load(std::memory_order_relaxed);

		if (SrcRate == DstRate)
		{
			Int16Data = Handle.samples;
			Frames = SrcFrames;
		}
		else
		{
			const double Ratio = static_cast<double>(SrcRate) / static_cast<double>(DstRate);
			Frames = static_cast<int32>(SrcFrames / Ratio);
			ResampleBuffer.SetNumUninitialized(Frames * SrcChannels, EAllowShrinking::No);

			for (int32 DstFrame = 0; DstFrame < Frames; ++DstFrame)
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

			Int16Data = ResampleBuffer.GetData();
		}

		const int32 TotalSrcSamples = Frames * SrcChannels;

		// --- Push to all outputs with per-output channel conversion ---
		FScopeLock Lock(&OutputsLock);
		for (FOutput& Out : Outputs)
		{
			if (!Out.ProceduralSound.IsValid())
			{
				continue;
			}

			FLink4UESoundGenerator* Gen = Out.ProceduralSound->GetGenerator();
			if (!Gen)
			{
				continue; // Generator not yet created by mixer
			}

			const int32 WaveChannels = (Out.ChannelFormat == ELink4UEChannelFormat::Mono) ? 1 : 2;

			if (SrcChannels == WaveChannels)
			{
				// Pass-through: convert int16→float and push
				FloatConvertBuffer.SetNumUninitialized(TotalSrcSamples, EAllowShrinking::No);
				constexpr float Scale = kInt16ToFloat;
				for (int32 i = 0; i < TotalSrcSamples; ++i)
				{
					FloatConvertBuffer[i] = Int16Data[i] * Scale;
				}
				Gen->PushAudio(FloatConvertBuffer.GetData(), TotalSrcSamples);
			}
			else if (SrcChannels == 1 && WaveChannels == 2)
			{
				// Mono→Stereo: duplicate + convert to float
				const int32 OutSamples = Frames * 2;
				FloatConvertBuffer.SetNumUninitialized(OutSamples, EAllowShrinking::No);
				constexpr float Scale = kInt16ToFloat;
				for (int32 i = 0; i < Frames; ++i)
				{
					const float S = Int16Data[i] * Scale;
					FloatConvertBuffer[i * 2]     = S;
					FloatConvertBuffer[i * 2 + 1] = S;
				}
				Gen->PushAudio(FloatConvertBuffer.GetData(), OutSamples);
			}
			else if (SrcChannels == 2 && WaveChannels == 1)
			{
				// Stereo→Mono: (L+R)/2 downmix + convert to float
				FloatConvertBuffer.SetNumUninitialized(Frames, EAllowShrinking::No);
				constexpr float Scale = kInt16ToFloat;
				for (int32 i = 0; i < Frames; ++i)
				{
					const int32 L = Int16Data[i * 2];
					const int32 R = Int16Data[i * 2 + 1];
					FloatConvertBuffer[i] = ((L + R) / 2) * Scale;
				}
				Gen->PushAudio(FloatConvertBuffer.GetData(), Frames);
			}
		}
	}

	Audio::FDeviceId ActiveDeviceId = INDEX_NONE;
	TArray<FOutput> Outputs;
	FCriticalSection OutputsLock;
	ableton::LinkAudio& LinkRef;
	const std::atomic<double>& QuantumRef;
	FString ChannelIdHex;
	std::atomic<int32> DeviceSampleRate;
	FString ChannelName;
	std::atomic<bool> bFlushNextCallback{false};
	ableton::LinkAudioSource Source; // Must be last — destructor stops callback first

	TArray<int16> ResampleBuffer;
	TArray<float> FloatConvertBuffer;

	// --- Latency stats (accumulated on SDK thread, drained on GameThread) ---
	struct FLatencyStats
	{
		std::atomic<int64> SumUs{0};
		std::atomic<int64> MaxUs{0};
		std::atomic<int32> Count{0};

		void Record(double LatencyMs)
		{
			const int64 Us = static_cast<int64>(LatencyMs * 1000.0);
			SumUs.fetch_add(Us, std::memory_order_relaxed);
			Count.fetch_add(1, std::memory_order_relaxed);
			int64 CurMax = MaxUs.load(std::memory_order_relaxed);
			while (Us > CurMax
				&& !MaxUs.compare_exchange_weak(CurMax, Us, std::memory_order_relaxed))
			{
			}
		}

		bool Drain(double& OutAvgMs, double& OutMaxMs, int32& OutCount)
		{
			OutCount = Count.exchange(0, std::memory_order_relaxed);
			if (OutCount == 0) return false;
			const int64 Sum = SumUs.exchange(0, std::memory_order_relaxed);
			OutMaxMs = MaxUs.exchange(0, std::memory_order_relaxed) / 1000.0;
			OutAvgMs = (Sum / static_cast<double>(OutCount)) / 1000.0;
			return true;
		}
	};
	FLatencyStats LatencyStats;
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
	struct FDesiredOutput { USoundSubmix* Submix; ELink4UEChannelFormat ChannelFormat; };
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
		DC.Outputs.Add({Submix, RecvDef.ChannelFormat});
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
				LinkInstance->Link, DC.Id, DeviceSampleRate, DC.Name, Quantum));
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
					&& Bridge->GetOutputSubmix(Ai) == DC.Outputs[Di].Submix
					&& Bridge->GetOutputChannelFormat(Ai) == DC.Outputs[Di].ChannelFormat)
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
			Bridge->AddOutput(DC.Outputs[Di].Submix, ActiveDevice, DC.Outputs[Di].ChannelFormat);
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

	// Report flush events and latency stats from receive bridges
	if (LinkInstance)
	{
		const int32 LatencyLogLevel = CVarLatencyLog.GetValueOnAnyThread();

		for (auto& Bridge : LinkInstance->ActiveReceives)
		{
			// Trim count (always reported when it happens)
			const int32 Trimmed = Bridge->DrainTrimCount();
			if (Trimmed > 0)
			{
				UE_LOG(LogLink4UE, Log,
					TEXT("Link4UE: '%s' trimmed excess %d time(s)"),
					*Bridge->GetChannelName(), Trimmed);
			}

			// Latency summary (level 1+, throttled to ~1 sec)
			if (LatencyLogLevel >= 1)
			{
				LatencyLogAccum += DeltaTime;
				if (LatencyLogAccum >= 1.0f)
				{
					LatencyLogAccum = 0.0f;
					double AvgMs, MaxMs;
					int32 Count;
					if (Bridge->DrainLatencyStats(AvgMs, MaxMs, Count))
					{
						UE_LOG(LogLink4UE, Log,
							TEXT("Link4UE [RECV] '%s': avg=%.1fms  max=%.1fms  (%d samples)"),
							*Bridge->GetChannelName(), AvgMs, MaxMs, Count);
					}
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

bool ULink4UESubsystem::AddAudioReceive(const FString& ChannelId, USoundSubmix* Submix,
	ELink4UEChannelFormat ChannelFormat)
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
	Recv.ChannelFormat = ChannelFormat;
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
