<div align="center">

# AirDeck-PC: developer guide

Build, architecture and protocol notes for the Windows server.

![C++](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![Qt](https://img.shields.io/badge/Qt-6.8-41CD52?style=flat-square&logo=qt&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-Ninja-064F8C?style=flat-square&logo=cmake&logoColor=white)

[Back to README](../README.md) | [Architecture](architecture.md) | [Protocol](protocol.md) | [Android app](https://github.com/horse-3903/AirDeck-Mobile)

</div>

---

## Tech Stack

![C++](https://img.shields.io/badge/C%2B%2B-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)
![Qt](https://img.shields.io/badge/Qt-41CD52?style=for-the-badge&logo=qt&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-064F8C?style=for-the-badge&logo=cmake&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-0078D6?style=for-the-badge&logo=windows&logoColor=white)
![JavaScript](https://img.shields.io/badge/JavaScript-F7DF1E?style=for-the-badge&logo=javascript&logoColor=black)
![HTML5](https://img.shields.io/badge/HTML5-E34F26?style=for-the-badge&logo=html5&logoColor=white)

The server is Qt 6 and C++20, built with CMake and Ninja on MSVC 2022. Virtual pads use [ViGEmClient](https://github.com/nefarius/ViGEmClient) (static, MIT). The browser controller is one HTML file with no dependencies. The wire format for the app is Colfer, from the `VGP_Data_Exchange` submodule.

---

## Getting Started

### Prerequisites

- Windows 10 or later
- Visual Studio 2022 with the C++ workload
- Qt 6.8.3 for MSVC 2022 64-bit
- CMake, Ninja and Git
- The [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases) driver, for the virtual controller mode

### Installation

```bash
git clone --recurse-submodules https://github.com/horse-3903/AirDeck-PC.git
cd AirDeck-PC
cmake -S . -B build-windows -G "Ninja Multi-Config" \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build-windows --config Release
cmake --install build-windows --prefix dist --config Release
```

Run these from a Visual Studio developer prompt so `cl` is on the path. The install step runs `windeployqt`. See [Build.md](Build.md) for details.

### Run locally

```bash
dist/bin/AirDeck.exe
```

`AirDeck.exe --ui-shots <folder>` saves a PNG of every page and dialog without touching the screen.

### Input modes

| Mode | Needs | Controllers | Notes |
|---|---|---|---|
| Virtual controller (ViGEm) | ViGEmBus driver | Up to 8 | Default. Xbox 360 or PlayStation (DualShock 4), set by Controller type. |
| InputInjector | Windows 10+, Administrator, Developer Mode | Up to 8 | Experimental. Crashes on some systems, but the server survives. |
| Keyboard and mouse | Nothing | 1 | Uses the active keymap profile. |

---

## Project Structure

```text
AirDeck-PC/
├── src/
│   ├── main.cpp              # entry point, theme, single-instance lock, --ui-shots
│   ├── networking/           # TCP server, protocols, executors
│   ├── ui/                   # main window, home, server page, preferences, tester
│   ├── settings/             # QSettings wrapper, keymap profiles, input types
│   ├── simulation/           # keyboard, mouse and gamepad injection
│   └── platform/             # elevation check, pad cleanup, native toast
├── res/
│   ├── web/controller.html   # the browser controller, one file
│   ├── controllers/          # controller SVG templates and status icons
│   └── theme.qss             # dark theme
├── VGP_Data_Exchange/        # Colfer wire protocol (submodule)
├── third-party-libs/         # ViGEmClient, QR code generator
├── cmake/                    # platform setup modules
└── Build.md                  # full build instructions
```

---

## Deployment

Releases are GitHub pre-releases, each with a Windows zip of `dist/bin` and a `SHA256SUMS.txt`. ViGEmBus is not bundled.

```bash
gh release create v0.6.0 AirDeck-PC-0.6.0-win64.zip SHA256SUMS.txt \
  --repo horse-3903/AirDeck-PC --prerelease --notes-file notes.md
```

---

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `CMAKE_PREFIX_PATH` | none | Qt kit path, for example `C:/Qt/6.8.3/msvc2022_64`. Required. |
| `CMAKE_CXX_COMPILER` | system default | Set to `cl` so CMake uses MSVC. |
| `CMAKE_INSTALL_PREFIX` | `C:/Program Files (x86)/Virtual Gamepad PC` | Install location. Use `--prefix dist` for a portable folder. |

### Runtime settings

Stored in `AirDeck.ini` next to the executable. Most are in Preferences.

| Setting | Default | Description |
|---|---|---|
| `server/port_v3` | `39268` | Listening port. A value outside 1024 to 65535 picks a random port each run. |
| `server/executor_type` | ViGEm | Input mode: ViGEm, InputInjector or keyboard and mouse. |
| `server/debug` | `false` | Writes `debug.log` under `%APPDATA%\horse-3903\AirDeck`, capped at 8 MiB. |
| `server/multi_controller` | `false` | Lets one device hold several controllers. |
| `server/pad_style` | `xbox` | Virtual controller type in ViGEm mode: `xbox` or `playstation`. |
| `server/require_approval` | `false` | Asks on the PC before a new device connects. |
| `server/trusted_devices` | empty | Addresses approved with Always allow. |
| `app/run_as_admin` | `false` | Starts elevated, so pad cleanup never prompts. |
| `mouse_setting/mouse_sensitivity` | `100` | Pointer speed in keyboard and mouse mode. |
