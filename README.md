# Link4UE

[Ableton Link](https://github.com/Ableton/link) and LinkAudio plugin for Unreal Engine.

Synchronizes musical beat, tempo, phase, and audio streams across multiple applications on a local network.

## Features

- **Link sync** — shared tempo, beat, phase, and transport (start/stop) across all Link peers
- **LinkAudio Send** — capture UE Submix audio and stream it to the Link network as mono/stereo channels
- **LinkAudio Receive** — subscribe to remote LinkAudio channels and inject audio into UE Submixes
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
| Windows  | Untested |
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

Open **Project Settings > Plugins > Link4UE**:

| Setting | Description |
|---------|-------------|
| Auto Connect | Join the Link network on engine startup |
| Start/Stop Sync | Synchronize transport state with other peers |
| Enable Link Audio | Enable LinkAudio channel streaming |
| Peer Name | Display name advertised to other peers |
| Default Tempo | Initial BPM for the session |
| Default Quantum | Phase synchronization unit (1 Bar, 1/4, etc.) |
| Audio Sends | Submix → LinkAudio channel routing |
| Audio Receives | LinkAudio channel → Submix routing |

## Third-Party

- [Ableton Link 4.0.0-beta.2](https://github.com/Ableton/link) (GPLv2+ / proprietary dual license)
  - [ASIO standalone 1.36.0](https://github.com/chriskohlhoff/asio) (Boost Software License)

## License

Link4UE is licensed under the [GNU General Public License v2 or later](LICENSE).

See also Ableton Link's [license](https://github.com/Ableton/link/blob/master/LICENSE.md) for details on the dual licensing model.
