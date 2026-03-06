# Link4UE

[Ableton Link](https://github.com/Ableton/link) plugin for Unreal Engine.

Synchronizes musical beat, tempo, and phase across multiple applications on a local network.

## Requirements

- Unreal Engine 5.7+
- C++20

## Supported Platforms

| Platform | Status |
|----------|--------|
| Windows  | Supported |
| macOS    | Supported |
| Linux    | Supported |

## Setup

This plugin is automatically cloned by the project's `setup-plugins.py` bootstrap script:

```bash
python setup-plugins.py
```

The Ableton Link library is included as a git submodule under `Source/ThirdParty/LinkLibrary/link/`. After cloning, initialize it with:

```bash
git submodule update --init --recursive
```

The bootstrap script handles this automatically.

## Third-Party

- [Ableton Link 3.1.5](https://github.com/Ableton/link) (GPLv2+ / proprietary dual license)
  - [ASIO standalone 1.36.0](https://github.com/chriskohlhoff/asio) (Boost Software License)

## License

See Ableton Link's [license](https://github.com/Ableton/link/blob/master/LICENSE.md) for details on the dual licensing model.
