---
description: Why Link4UE tracks a single active AudioDevice and recreates audio routes on PIE/editor device transitions; read when PIE-end kills editor audio or when touching device-transition / route-recreation logic.
type: decision
status: implemented
updated: 2026-03-10
---

# Active Device Only Architecture — PIE Audio Survival

> Status: **Implemented** — ba4b1ed (2026-03-10)
> Related: [audio-pipeline-redesign.md](audio-pipeline-redesign.md)

## Problem

UE5 maintains multiple AudioDevice instances simultaneously:

| Context      | Device ID | Lifetime                |
|-------------|-----------|-------------------------|
| Editor       | 1         | Process lifetime        |
| PIE          | 2+        | PIE session             |
| Standalone   | 1         | Process lifetime        |

When PIE ends, Device 2 is destroyed. This destruction **kills audio on Device 1** (the editor device) as a side effect. Link4UE's receive routes — `USoundWaveProcedural` instances playing via `AudioComponent::Play()` — go silent.

### Root Cause Analysis

The exact mechanism remains unidentified, but investigation narrowed it to the following candidates:

1. **`FAudioDevice::Flush(nullptr)`** — bypasses `bIgnoreForFlushing` flag (confirmed in UE source `AudioDevice.cpp:6805`). If called during Device 2 teardown, it can stop sounds on all devices.

2. **`MarkPendingDestroy` via `IterateOverAllDevices`** — Device 2's cleanup may iterate all devices and mark ActiveSounds for destruction on Device 1.

3. **`StopSourcesUsingBuffer`** — cross-device API that stops any source using a given USoundWave buffer, regardless of which device the source belongs to.

4. **GC interleaving** — PIE world GC may collect UObjects that Device 1's ActiveSounds reference, if those objects were created without explicit outer ownership.

### Ruled Out

- **`au.Debug.PlayAllPIEAudio`** — tested; does NOT prevent the issue. This rules out the focus/solo audio policy as the cause.

## Solution: Active Device Only

Instead of outputting to all devices simultaneously, track only the **single active device** and recreate routes on device transitions.

### Key Design Decisions

**Why not output to all devices?**
Outputting to every AudioDevice (editor + PIE + standalone simultaneously) was considered but rejected:
- Doubles CPU cost for audio processing during PIE
- Doesn't solve the core problem — Device 2 teardown still kills Device 1's sounds
- Semantically wrong — PIE audio should come from PIE device, editor audio from editor device

**Why Tick-based monitoring instead of callback-only?**
`OnAudioDeviceCreated`/`Destroyed` callbacks alone are insufficient:
- The active device ID may not change (Device 1 persists), but its sounds are still killed
- The `bNeedsAudioRecreation` flag bridges this gap: set by `OnAudioDeviceDestroyed`, consumed by the next Tick to trigger recreation even when `CurrentActiveDeviceId` hasn't changed

**Why `GetTransientPackage()` as outer?**
`USoundWaveProcedural` objects created with bare `NewObject<>()` (no outer) may be associated with the PIE world and garbage-collected when PIE ends. Using `GetTransientPackage()` as the outer ensures they survive PIE teardown.

### Architecture

```
Tick (GameThread)
  │
  ├── Detect active device change (GetMainAudioDeviceRaw)
  │   └── CurrentActiveDeviceId != NewActiveId
  │       → RecreateAudioOnDevice(NewActiveId)
  │
  └── Detect device destruction recovery
      └── bNeedsAudioRecreation == true
          → RecreateAudioOnDevice(CurrentActiveDeviceId)
```

#### RecreateAudioOnDevice

1. **Send bridges** (Submix → LinkAudio): Unregister `SubmixBufferListener` from old device, re-register on new device
2. **Receive bridges** (LinkAudio → Submix): Call `SwitchToDevice()` — stops old `AudioComponent`, creates new `USoundWaveProcedural` + `UAudioComponent` on the target device, resumes playback

#### FLink4UEReceiveBridge — Single-Device Model

Previous design held a `TMap<FDeviceId, FDeviceOutput>` to support multiple devices. This was flattened to a single `ActiveDeviceId` + one `USoundWaveProcedural` / `UAudioComponent` pair.

- `SwitchToDevice(DeviceId)` — tear down current, create on new device
- `Deactivate()` — tear down without creating new (used when current device is destroyed)

### Lifecycle

```
Editor boot
  → Initialize() registers OnAudioDeviceCreated/Destroyed callbacks
  → First Tick detects Device 1, calls RebuildAudioSends/Receives

PIE start
  → OnAudioDeviceCreated(Device 2) — sets bNeedsAudioRecreation
  → Tick detects new active device → RecreateAudioOnDevice(Device 2)

PIE end
  → OnAudioDeviceDestroyed(Device 2) — Deactivate() receives, sets bNeedsAudioRecreation
  → Tick detects bNeedsAudioRecreation → RecreateAudioOnDevice(Device 1)
  → Audio resumes on editor device
```

## Future Considerations

- If the root cause is definitively identified in a future UE version, the recreation mechanism may become unnecessary — but it remains a safe and correct pattern regardless
- `bIgnoreForFlushing = true` on AudioComponents may provide additional protection but was not tested in isolation
