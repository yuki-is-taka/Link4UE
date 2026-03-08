# Link4UE

[Ableton Link](https://github.com/Ableton/link) and Link Audio plugin for Unreal Engine.

Synchronizes musical beat, tempo, phase, and audio streams across multiple applications on a local network.

## Features

- **Link sync** — shared tempo, beat, phase, and transport (start/stop) across all Link peers
- **Link Audio Send** — capture UE Submix audio and stream it to the Link network as mono/stereo channels
- **Link Audio Receive** — subscribe to remote Link Audio channels and inject audio into UE Submixes
- **Beat delegates** — `OnBeat` and `OnPhaseZero` events fired on GameThread for gameplay synchronization
- **Channel discovery** — `GetChannels()` and `OnChannelsChanged` for listing available audio channels
- **Editor settings** — `UDeveloperSettings` with hot-reload; audio routing configured via Project Settings
- **Blueprint API** — all Link controls (tempo, quantum, transport, audio routing) exposed as `BlueprintCallable`

## Requirements

- Unreal Engine 5.7+
- C++20

## Supported Platforms

| Platform | Status |
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

Settings are stored locally in `Saved/Config/{Platform}/Link4UE.ini` (not version-controlled).

### Connection

| Setting | Default | Description |
|---------|---------|-------------|
| Auto Connect | `true` | Join the Link network on engine startup |
| Start/Stop Sync | `true` | Synchronize transport state with other peers |
| Enable Link Audio | `false` | Enable Link Audio channel streaming |
| Peer Name | `Unreal` | Display name advertised to other peers |

### Defaults

| Setting | Default | Description |
|---------|---------|-------------|
| Default Tempo | `120.0` | Initial BPM for the session (shared once peers connect) |
| Default Quantum | `1 Bar` | Phase synchronization unit (matches Ableton Live's global quantize options) |

### Audio Routing

| Setting | Description |
|---------|-------------|
| Audio Sends | Submix → Link Audio channel routing |
| Audio Receives | Link Audio channel → Submix routing |

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

### Sending Audio (UE → Network)

Add entries to **Audio Sends** in Project Settings:

| Field | Description |
|-------|-------------|
| Submix | The UE Submix to capture. Audio flowing through this Submix is streamed to the network |
| Channel Name Prefix | Name shown to remote peers. Empty = Submix asset name |

Each entry creates a Link Audio channel that remote peers can subscribe to. Stereo only (Link Audio SDK constraint).

### Receiving Audio (Network → UE)

Add entries to **Audio Receives** in Project Settings:

| Field | Description |
|-------|-------------|
| Channel | Dropdown of available remote channels. Requires Link Audio enabled and peers connected |
| Submix | Target Submix for playback. Empty = Master Submix |

Channels are identified by a stable ID internally. If a peer renames a channel (e.g. renaming a track in Ableton Live), the routing stays intact and the display name auto-updates.

Multiple receives can reference the same remote channel with different target Submixes — the audio is shared without duplication overhead.

### Channel Discovery

| Function / Delegate | Description |
|---------------------|-------------|
| `GetChannels()` | Returns all currently visible remote channels, each with ChannelId, Name, PeerId, and PeerName |
| `OnChannelsChanged` | Fires when a channel appears, disappears, or is renamed |

### Runtime Controls

| Function | Description |
|----------|-------------|
| `EnableLinkAudio(bEnable)` | Toggle Link Audio at runtime |
| `SetPeerName(Name)` | Change the advertised peer name |

### Latency

The end-to-end latency is approximately **45–67 ms** (LAN). This is suitable for gameplay synchronization and BGM streaming, but not for real-time performance monitoring. See [Docs/audio-pipeline-redesign.md](Docs/audio-pipeline-redesign.md) for a detailed latency breakdown.

---

## Third-Party

- [Ableton Link 4.0.0-beta.2](https://github.com/Ableton/link) (GPLv2+ / proprietary dual license)
  - [ASIO standalone 1.36.0](https://github.com/chriskohlhoff/asio) (Boost Software License)

## License

Link4UE is licensed under the [GNU General Public License v2 or later](LICENSE).

See also Ableton Link's [license](https://github.com/Ableton/link/blob/master/LICENSE.md) for details on the dual licensing model.
