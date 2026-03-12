# Link4UE

[Ableton Link](https://github.com/Ableton/link) and Link Audio plugin for Unreal Engine.

Synchronizes musical beat, tempo, phase, and audio streams across multiple applications on a local network.

## Features

- **Link sync** — shared tempo, beat, phase, and transport (start/stop) across all Link peers
- **Link Audio Send** — capture UE Submix audio and stream it to the Link network as mono/stereo channels
- **Link Audio Receive** — subscribe to remote Link Audio channels and inject audio into UE Submixes
- **Beat delegates** — `OnBeat` and `OnPhaseZero` events fired on GameThread for gameplay synchronization
- **Channel discovery** — `GetChannels()` and `OnChannelsChanged` for listing available audio channels
- **Editor settings** — Engine Subsystem with Config persistence; audio routing configured via Project Settings
- **Blueprint API** — all Link controls (tempo, quantum, transport, audio routing) exposed as `BlueprintCallable`

## Requirements

- Unreal Engine 5.7+
- C++20

## Supported Platforms

| Platform | UE 5.7 |
|----------|--------|
| macOS    | Builds |
| Windows  | Builds |
| Linux    | Untested |

## Setup

Clone into your project's `Plugins/` directory and initialize submodules:

```bash
git clone https://github.com/yuki-is-taka/Link4UE.git Plugins/Link4UE
cd Plugins/Link4UE
git submodule update --init --recursive
```

The `--recursive` flag is required because Ableton Link itself contains a submodule (ASIO standalone).

## Configuration

Open **Project Settings > Plugins > Link4UE**.

Settings appear in the Project Settings UI but are stored per-user in `Saved/Config/{Platform}/EditorPerProjectUserSettings.ini` (not version-controlled). This means each team member has their own Link4UE configuration — changing settings does not affect other users' setups.

### Connection

| Setting | Description |
|---------|-------------|
| Auto Connect | Join the Link network on engine startup |
| Start/Stop Sync | Synchronize transport state with other peers |
| Enable Link Audio | Enable Link Audio channel streaming |
| Peer Name | Display name advertised to other peers |

### Defaults

| Setting | Description |
|---------|-------------|
| Default Tempo | Initial BPM for the session (shared once peers connect) |
| Default Quantum | Phase synchronization unit (matches Ableton Live's global quantize options) |

### Audio Routing

| Setting | Description |
|---------|-------------|
| Audio Sends | Submix → Link Audio channel routing |
| Audio Receives | Link Audio channel → Submix routing |

### Config Persistence

All settings listed above are `UPROPERTY(Config)` and stored in `EditorPerProjectUserSettings.ini`.

| Edit method | Saved to ini? | Editor UI updated? |
|-------------|---------------|-------------------|
| **Project Settings UI** (editor) | Immediately | Yes |
| **Blueprint / C++ mutator** | No — call `SaveConfig()` to persist | Yes (via `PostEditChangeProperty`) |

Blueprint mutators (`EnableLinkAudio`, `SetPeerName`, `EnableStartStopSync`, `AddAudioSend`, `RemoveAudioSend`, `ClearAudioSends`, `AddAudioReceive`, `RemoveAudioReceive`, `ClearAudioReceives`) apply changes immediately at runtime and refresh the editor UI, but **do not write to ini**. If you need changes to survive an editor restart, call `SaveConfig()` on the subsystem after making changes.

The following Config properties are affected:

| Property | Mutator(s) |
|----------|-----------|
| `bAutoConnect` | (editor only) |
| `bStartStopSync` | `EnableStartStopSync(bEnable)` |
| `bEnableLinkAudio` | `EnableLinkAudio(bEnable)` |
| `PeerName` | `SetPeerName(Name)` |
| `DefaultTempo` | (editor only — runtime tempo is set via `SetTempo`) |
| `DefaultQuantum` | (editor only — runtime quantum is set via `SetQuantum`/`SetQuantumPreset`) |
| `AudioSends` | `AddAudioSend`, `RemoveAudioSend`, `ClearAudioSends` |
| `AudioReceives` | `AddAudioReceive`, `RemoveAudioReceive`, `ClearAudioReceives` |

> **Note:** `SetTempo`, `SetQuantum`, `SetQuantumPreset`, `SetIsPlaying`, and `RequestBeatAtTime` are **session-only controls** that modify the live Link state. They do not write to Config properties and are not persisted.

---

## Link (Beat/Tempo Sync)

Link provides a shared musical timeline. All Link-enabled apps on the same network (Ableton Live, other DAWs, mobile apps, etc.) automatically discover each other — no manual pairing needed.

The subsystem (`ULink4UESubsystem`) is an Engine Subsystem. Access it from Blueprints via **Get Engine Subsystem > Link4UE Subsystem**.

### Session Snapshot

`GetSessionSnapshot` returns the current state of the Link session, updated every frame:

| Field | Type | Description |
|-------|------|-------------|
| Tempo | double | Current BPM, shared across all peers |
| Beat | double | Fractional beat position on the shared timeline |
| Phase | double | Position within the current quantum `[0, Quantum)` |
| bIsPlaying | bool | Transport state (requires Start/Stop Sync) |
| NumPeers | int32 | Number of connected peers (excluding self) |
| Quantum | double | Current quantum value in beats |

### Events

| Delegate | Fires when... |
|----------|--------------|
| `OnBeat(BeatNumber, bIsPhaseZero)` | A beat boundary is crossed. `bIsPhaseZero` is true at the start of each bar |
| `OnPhaseZero` | A bar boundary is crossed (phase resets to 0) |
| `OnTempoChanged(BPM)` | Any peer changes the tempo |
| `OnStartStopChanged(bIsPlaying)` | Transport state changes |
| `OnNumPeersChanged(NumPeers)` | A peer joins or leaves the session |

All delegates fire on GameThread and are `BlueprintAssignable`.

### Controls

| Function | Description |
|----------|-------------|
| `SetTempo(BPM)` | Set the session tempo (propagates to all peers) |
| `SetIsPlaying(bPlay)` | Start or stop transport (requires Start/Stop Sync) |
| `RequestBeatAtTime(Beat)` | Reset the beat position (e.g. pass `0` to restart from beat 0) |
| `SetQuantum(Beats)` | Set the quantum as a raw beat value |
| `SetQuantumPreset(Preset)` | Set the quantum from a preset (1 Bar, 1/4, 1/8, etc.) |
| `EnableLink()` / `DisableLink()` | Connect or disconnect from the Link network |
| `EnableStartStopSync(bEnable)` | Toggle transport sync |

### Scheduling Ahead

`GetTimeAtBeat(Beat)` returns the wall-clock time (in seconds) at which a given beat will occur. Useful for scheduling events ahead of time, such as triggering a visual effect exactly at the next bar.

---

## Link Audio (Audio Streaming)

Link Audio extends Link with real-time audio streaming between peers. UE can send Submix audio to the network and receive remote channels into Submixes.

### Prerequisites

1. **Enable Link Audio** must be on in settings (or call `EnableLinkAudio(true)`)
2. The remote peer (e.g. Ableton Live) must also have Link Audio enabled
3. Both peers must be on the same local network

### Master Send

When Link Audio is enabled, a **Master Send** is automatically created. This captures the Master Submix output and advertises it as the "Main" channel on the network.

This is required by the Link Audio protocol — without at least one Send, remote peers cannot discover this node and will not transmit audio. The Master Send is always active independently of user-configured sends.

### Channel Format Philosophy

Link4UE follows a simple principle: **send what you have, convert at the receiver**.

The send side always transmits the Submix's native channel count (up to stereo). No format option is exposed — the sender does not discard information. The receive side has a **Channel Format** setting (Mono / Stereo) that determines how incoming audio is consumed within UE. Conversion between formats happens automatically at the Wave boundary.

This mirrors how DAWs handle routing: a send bus outputs its native format, and the return track decides its own channel configuration.

### Sending Audio (UE → Network)

Add entries to **Audio Sends** in Project Settings:

| Field | Description |
|-------|-------------|
| Submix | The UE Submix to capture. Audio flowing through this Submix is streamed to the network |
| Channel Name Prefix | Name shown to remote peers. Empty = Submix asset name |

Each entry creates a Link Audio channel. The channel count is determined dynamically per audio callback from the Submix's actual output:

| Submix channels | Sent as | Notes |
|-----------------|---------|-------|
| 1 (mono) | 1ch | Passed through as-is |
| 2 (stereo) | 2ch | Passed through as-is |
| 3+ (surround) | 2ch | First 2 channels (L, R) extracted. Not a proper downmix — Center, LFE, surround channels are discarded |

In practice, UE Submixes are almost always stereo. The 3+ channel case is a safety fallback, not a surround downmix feature.

### Receiving Audio (Network → UE)

Add entries to **Audio Receives** in Project Settings:

| Field | Description |
|-------|-------------|
| Channel | Dropdown of available remote channels. Requires Link Audio enabled and peers connected |
| Channel Format | Mono or Stereo. Determines the internal Wave's channel count |
| Submix | Target Submix for playback. Empty = Master Submix |

Channels are identified by a stable ID internally. If a peer renames a channel (e.g. renaming a track in Ableton Live), the routing stays intact and the display name auto-updates.

Multiple receives can reference the same remote channel with different target Submixes — the audio is shared without duplication overhead.

#### Channel Format

Channel Format works like a DAW track's channel setting. It determines the `USoundWaveProcedural`'s channel count, which is fixed for the lifetime of the Wave.

| Channel Format | Wave channels | Use case |
|----------------|---------------|----------|
| **Stereo** (default) | 2 | Pre-panned audio, standard playback |
| **Mono** | 1 | 3D spatialization via `UAudioComponent` + `SetSound()` |

If the remote channel's actual format differs from the Wave's format, conversion happens automatically per callback:

| Remote → Wave | Conversion |
|---------------|------------|
| Stereo → Stereo | Pass-through |
| Mono → Mono | Pass-through |
| Mono → Stereo | L = R = mono sample (unity gain, DAW convention) |
| Stereo → Mono | (L + R) / 2 downmix |

The remote channel's format can change at any time (e.g. if the sender switches from a stereo to a mono Submix). The conversion adapts dynamically per callback without requiring a rebuild.

### Channel Discovery

| Function / Delegate | Description |
|---------------------|-------------|
| `GetChannels()` | Returns all currently visible remote channels, each with ChannelId, Name, PeerId, and PeerName |
| `OnChannelsChanged` | Fires when a channel appears, disappears, or is renamed |

### Latency Tip

UE's audio callback buffer size defaults to 1024 frames. Lowering this reduces Link Audio latency at the cost of higher CPU usage. The Project Settings UI clamps to 512–4096, but you can go as low as 240 by editing the platform ini directly:

```ini
; e.g. Config/Mac/MacEngine.ini
[/Script/MacTargetPlatform.MacTargetSettings]
AudioCallbackBufferFrameSize=256
```

> **macOS caveat:** The CoreAudio backend in UE hardcodes the audio callback buffer size to **1024 frames** regardless of `AudioCallbackBufferFrameSize`. This means on Mac the minimum achievable audio callback latency is ~21.3 ms at 48 kHz, compared to ~5.3 ms (256 frames) on Windows/iOS where the setting is respected. This is an engine limitation, not a Link4UE issue.

### Jitter Buffer (Receive)

The receive path includes a jitter buffer that absorbs network and scheduling jitter. The console variable `Link4UE.JitterBuffer` controls the operating mode:

| Value | Mode | Behavior |
|-------|------|----------|
| **-1** (default) | Adaptive | Grows the buffer quickly on underrun, shrinks it slowly during stable periods |
| **0** | Min latency | Fixed to 1 render cycle (lowest latency but vulnerable to underruns) |
| **N** (positive int) | Fixed | Fixed target of N milliseconds |

#### Adaptive Mode

Adaptive mode uses **asymmetric adaptation**:

- **Fast increase** — on underrun, the target grows by the deficit plus 1 render cycle of safety margin. The per-event increase is capped at 2 render cycles to prevent runaway growth under periodic jitter.
- **Slow decrease** — after ~5 seconds of consecutive stable callbacks (no underruns), the target shrinks by 1/4 render cycle.
- **Cold-start protection** — adaptation is disabled until the first audio data arrives.

#### Buffer Bounds

Example values at 48 kHz stereo with a 1024-frame callback (~21.3 ms):

| Parameter | Value | Time |
|-----------|-------|-----:|
| Initial target | 2 render cycles | ~42.7 ms |
| Min target | 1 render cycle | ~21.3 ms |
| Max target | SampleRate × NumChannels / 2 | 500 ms |
| Ring buffer capacity | SampleRate × NumChannels | 1 s |

#### Consumer-Side Trim

The producer (SDK callback) only pushes samples into the ring buffer and never manages buffer state. The consumer (`ISoundGenerator::OnGenerateAudio`) discards excess samples from the head of the buffer each callback, keeping the buffer level at the target and preventing latency accumulation.

Trim events are counted via `TrimCount` and reported in log output.

### Runtime Controls

| Function | Description |
|----------|-------------|
| `EnableLinkAudio(bEnable)` | Toggle Link Audio at runtime |
| `SetPeerName(Name)` | Change the advertised peer name |

---

## Third-Party

- [Ableton Link 4.0.0-beta.2](https://github.com/Ableton/link) (GPLv2+ / proprietary dual license)
  - [ASIO standalone 1.36.0](https://github.com/chriskohlhoff/asio) (Boost Software License)

## License

Link4UE is licensed under the [GNU General Public License v2 or later](LICENSE).

See also Ableton Link's [license](https://github.com/Ableton/link/blob/master/LICENSE.md) for details on the dual licensing model.
