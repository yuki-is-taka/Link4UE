# Audio Pipeline Redesign — SoundWaveProcedural Approach

> Status: **Implemented** — Phases 1–3 complete, integration tested
> Date: 2026-03-08
> Related: [Link Audio SDK 4.0.0-beta2](https://github.com/Ableton/link)

## Background

The previous AudioBus + PatchInput approach had the following issues:

- Required users to manually create **AudioBus** and **SoundSourceBus** assets
- Playing through SoundSourceBus required additional steps (placing AmbientSound, calling PlaySound2D, etc.)
- Too many steps between configuration UI and actual playback — poor UX
- AudioBus PatchInput/PatchMixer is designed for internal audio engine routing, not external audio injection

Investigation of AudioCaptureComponent revealed that **USoundWaveProcedural** with direct buffer injection is the canonical UE pattern for this use case.

Why not SoundSourceBus? `USoundSourceBus` derives from `USoundWave` (not `USoundWaveProcedural`) and has no `QueueAudio()`. Data injection requires three intermediate objects (`AddPatchInputForAudioBus` → `FPatchInput` → `FPatchMixer`), all defined in `MultithreadedPatching.h`. By contrast, USoundWaveProcedural supports direct buffer injection via `QueueAudio()` (internal TQueue).

---

## Architecture Overview

### Receive (Link Audio → UE)

```
Link Audio Source callback (Link thread, int16 interleaved)
    |
    v
[Resampling (linear interpolation, only when SrcRate != DeviceRate)]
    |
    v
USoundWaveProcedural::QueueAudio()   <-- thread-safe (internal TQueue)
    |
    v
Audio Mixer: GeneratePCMData() dequeues (audio thread)
    |
    v
FActiveSound → USoundSubmix → Speaker output
```

FActiveSound is constructed directly and registered with `FAudioDevice::AddNewActiveSound()`. No UAudioComponent, World, Actor, or Component needed. `SetWorld(nullptr)` is safe (guarded by `&&` checks), `bAllowSpatialization=false` for non-spatialized audio, `bIsUISound=true` to decouple from World lifetime. Submix routing via `SetSubmixSend()`.

The Master Send (auto Sink) is required for the SDK peer discovery protocol — see [link-audio-sink-requirement.md](link-audio-sink-requirement.md). It has no impact on the receive pipeline.

### Send (UE → Link Audio) — Unchanged

```
USoundSubmix → ISubmixBufferListener::OnNewSubmixBuffer() → float→int16 → Sink::commit()
```

---

## Settings

```cpp
USTRUCT(BlueprintType)
struct FLink4UEAudioSend
{
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    TSoftObjectPtr<USoundSubmix> Submix;    // Empty = Master Submix

    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    FString ChannelNamePrefix;              // Empty = Submix asset name
};

USTRUCT(BlueprintType)
struct FLink4UEAudioReceive
{
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    FString ChannelId;                      // Stable hex ID, set by dropdown

    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    FString ChannelName;                    // Display name, auto-updated on rename

    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    TSoftObjectPtr<USoundSubmix> Submix;    // Empty = Master Submix
};
```

- Settings use `Config = Link4UE` → stored locally in `Saved/Config/{Platform}/Link4UE.ini` (not version-controlled)
- Editor customization: `FLink4UEAudioReceiveCustomization` replaces ChannelId/ChannelName fields with a dropdown populated from `GetChannels()`. Writes both ChannelId and ChannelName on selection. Initial selection resolves by ID first, name as fallback. Submix uses the default UE asset picker.

---

## ReceiveBridge Design

### 1 Channel = 1 Bridge (ID-Based)

The SDK's `MainProcessor::receiveAudioBuffer()` uses `std::find_if()` to deliver the buffer to only the first matching Source. Multiple Sources for the same ChannelId would leave subsequent ones silent.

Therefore **one ReceiveBridge (= one LinkAudioSource) per Link Audio channel**. When the same channel needs multiple Submix outputs, a single Bridge holds multiple FOutput entries and broadcasts `QueueAudio()` to all of them in the callback.

Bridges are keyed by **ChannelId (hex)**, not channel name. The SDK's ChannelId (8-byte NodeId) is stable across peer renames. `SyncChannelNames()` auto-updates stored names when the `channelsChanged` callback fires.

### Lifetime and Teardown

`USoundWaveProcedural` is a UObject (GC-managed). Since ReceiveBridge is a plain C++ class, `TStrongObjectPtr<T>` prevents premature GC — the same pattern used by `UAudioBusSubsystem` for AudioBus instances.

`FActiveSound` is not a UObject. Ownership transfers to AudioDevice via `AddNewActiveSound()`. To stop playback, use `StopSoundsUsingResource(USoundWave*)` — each ProceduralSound is unique per output, so no risk of stopping unrelated sounds.

Teardown order is critical:

```
1. ~LinkAudioSource → SDK callback stops (member destruction order guarantees this)
2. StopSoundsUsingResource → mixer stops calling GeneratePCMData()
3. ~TStrongObjectPtr → USoundWaveProcedural becomes GC-eligible
```

Violating this order can crash if GeneratePCMData() runs after ProceduralSound is destroyed.

### Class Definition

```cpp
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
                          const FString& InChannelName);
    ~FLink4UEReceiveBridge();

    void AddOutput(USoundSubmix* TargetSubmix, FAudioDevice* AudioDevice);
    void RemoveOutput(int32 Index, FAudioDevice* AudioDevice);
    int32 GetNumOutputs() const;
    USoundSubmix* GetOutputSubmix(int32 Index) const;
    const FString& GetChannelIdHex() const;
    void SetChannelName(const FString& NewName);

private:
    void OnSourceBuffer(ableton::LinkAudioSource::BufferHandle Handle);

    TArray<FOutput> Outputs;
    FString ChannelIdHex;
    int32 DeviceSampleRate;
    FString ChannelName;
    double CreationTime;
    ableton::LinkAudioSource Source; // Must be last — destructor stops callback first

    TArray<int16> ResampleBuffer;   // Safe as member: SDK callback is serial (single ASIO thread)
    std::atomic<bool> bLoggedFirstCallback{false};
    std::atomic<uint64> CallbackCount{0};
};
```

### Diff-Based Rebuild

Both sends and receives use diff-based reconciliation to avoid audio dropout on settings changes:

1. Build desired state from current settings
2. Match desired ↔ active (first unmatched match wins, preserves duplicates)
3. Remove unmatched active entries (reverse iteration for stable indices)
4. Add unmatched desired entries

For receives, the diff operates at two levels:
- **Bridge level**: keyed by ChannelIdHex — remove bridges whose ID is no longer desired, create new ones
- **Output level**: within each bridge, match by target Submix — remove/add as needed
- If a bridge ends up with zero outputs, it is destroyed

Guards:
- `OnSettingsChanged` receives the property name → only rebuilds the affected side (sends or receives)
- `bIsRebuilding` prevents recursive rebuilds when `bChannelsDirty` fires during our own Sink/Source creation
- `bChannelsDirty` is drained after rebuild
- Master Send is preserved across user send changes (split teardown)

### Resampling

`USoundWaveProcedural::SetSampleRate()` only sets metadata — the UE mixer does not auto-resample between source and device rates. Queuing 96 kHz data for a 48 kHz device results in half-speed playback (confirmed via UE 5.7 source: `FMixerSourceBufferInitArgs::SampleRate` always receives `AudioDevice->GetSampleRate()`).

The callback performs manual linear interpolation resampling when `SrcRate != DeviceSampleRate`. This handles any ratio (96→48, 44100→48000, etc.). Pitch correction via `SetPitchMultiplier()` was rejected due to side effects with attenuation and Doppler.

### Data Format

USoundWaveProcedural expects int16 by default (`GetGeneratedPCMDataFormat()` → `Int16`). The SDK's int16 interleaved PCM can be passed directly via `reinterpret_cast<const uint8*>()`. The mixer auto-converts to float32 via `Audio::ArrayPcm16ToFloat()`.

### Thread Safety

The SDK callback runs serially on a single ASIO thread ("Link Main"). `ResampleBuffer` as a member variable is safe — no synchronization needed. `QueueAudio()` is internally thread-safe (TQueue), so the ASIO thread → audio thread handoff is also safe.

---

## Latency Budget

The UE audio mixer introduces approximately 2–3 callback cycles of latency.

**UE-side breakdown** (48 kHz, default settings):

| Stage | Latency | Notes |
|-------|---------|-------|
| QueueAudio → GeneratePCMData | 0–21.3 ms | Wait for next mixer callback (worst case 1 cycle) |
| One mixer callback | 21.3 ms | CallbackBufferFrameSize = 1024 frames @ 48 kHz |
| Platform output buffer | 21.3 ms × 2 | NumBuffers = 2 (XAudio2 double buffering) |

**Link Audio side**:
- Network (UDP): ~1 ms (LAN)
- SDK encode/decode + process timer: ~2 ms

**Total: ~45–67 ms**. Acceptable for gameplay sync and BGM streaming. For lower latency, reduce `AudioCallbackBufferFrameSize` to 256 (~75% reduction in UE-side latency).

---

## Removed Components

- `FLink4UEAudioReceive::AudioBus` (`TSoftObjectPtr<UAudioBus>`)
- `#include "AudioBusSubsystem.h"` and all `UAudioBusSubsystem` usage
- `Audio::FPatchInput` related code
- Channel remapping logic (UE mixer handles channel count natively)
