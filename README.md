# Link4UE

[Ableton Link](https://github.com/Ableton/link) plugin for Unreal Engine.

Synchronizes musical beat, tempo, and phase across multiple applications on a local network.

## Requirements

- Unreal Engine 5.7+
- C++20

## Supported Platforms

| Platform | Status |
|----------|--------|
| Windows  | Untested |
| macOS    | Untested |
| Linux    | Untested |

## Setup

Clone into your project's `Plugins/` directory and initialize submodules:

```bash
git clone https://github.com/yuki-is-taka/Link4UE.git Plugins/Link4UE
cd Plugins/Link4UE
git submodule update --init --recursive
```

The `--recursive` flag is required because Ableton Link itself contains a submodule (ASIO standalone).

## Third-Party

- [Ableton Link 4.0.0-beta.2](https://github.com/Ableton/link) (GPLv2+ / proprietary dual license)
  - [ASIO standalone 1.36.0](https://github.com/chriskohlhoff/asio) (Boost Software License)

## License

See Ableton Link's [license](https://github.com/Ableton/link/blob/master/LICENSE.md) for details on the dual licensing model.
