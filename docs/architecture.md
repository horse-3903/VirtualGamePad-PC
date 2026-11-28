# Architecture

AirDeck-PC is a Qt 6 and C++20 desktop app. It accepts phone connections, decodes controller state, and hands it to an executor that creates input Windows understands.

## Components

| Part | Where | Job |
|---|---|---|
| Server | `src/networking/server.*` | TCP listener and server page. Sniffs each connection, keeps a `Client` per phone (protocol, slot, name, model, executor), hosts the browser page and a hand-written WebSocket. |
| Executors | `src/networking/executor.*` | Turn controller state into input. `ViGEmExecutor` is a virtual Xbox 360 or DualShock 4 pad, `GamepadExecutor` uses Windows InputInjector, `KeyboardMouseExecutor` presses keys. |
| UI | `src/ui/` | Main window, home, server page, preferences, input tester, and the tray icon. |
| Settings | `src/settings/` | `AirDeck.ini` and `profiles/` next to the exe. Input type, keymap profiles. |
| Platform | `src/platform/` | Elevation check, virtual pad cleanup, native toasts. |
| Debug log | `src/debuglog.*` | Bounded log, 8 MiB with one rotated copy, mirrors everything the app logs in debug mode. |
| Browser controller | `res/web/controller.html` | One self-contained page, embedded as a Qt resource and served by the server. |

## Data flow

1. A phone connects. The server reads the first bytes and picks the app or browser path.
2. It assigns the lowest free slot (1 to 8), creates an executor for that client and replies with the slot.
3. Each reading from the phone goes to the client's executor. ViGEm pads are updated, or keys and mouse events are injected.
4. For ViGEm, each pad registers a rumble notification. The driver thread calls back, the server hops to the interface thread with a queued call and writes `VGPRUMBLE` or a JSON `rumble` message to that slot's client.
5. On disconnect the executor releases everything it holds, so no key stays pressed.

One controller per device address is enforced unless Allow several controllers per device is on.

## Input modes

- **ViGEm** needs the ViGEmBus driver. `server/pad_style` picks the pad type: `xbox` (the default) gives slots 1 to 4 Xbox 360 (XInput) and 5 to 8 DualShock 4 style, and `playstation` makes all eight DualShock 4. The style is read when a device connects.
- **InputInjector** is experimental. Creation is wrapped in SEH, so a crash in `xboxgipsynthetic.dll` does not take the server down.
- **Keyboard and mouse** tracks held keys per source and only presses or releases on transitions, so a key mapped from several controls is reference counted.

## Server lifecycle

- `stopServer()` closes clients and the listener but stays on the page.
- `restartServer()` confirms if clients are connected, drops them, waits 700 ms and listens again on the configured port.
- `initServer()` and the restart timer call `releaseAllPads(done)`, which removes leftover virtual pads off the interface thread and pauses accepting connections until done.
- Pad cleanup uses `pnputil /remove-device <id> /subtree` as administrator, children before parents. `removeVirtualPadsElevated()` relaunches the exe with `--clean-pads` through UAC.
- Windows can give the first pad an XInput number other than 1. The executor probes the number, and the server restarts once if controller 1 is not XInput 1.

## Browser controller

- The stage is 2000 by 900 CSS pixels, scaled by `--k` and rotated by `--rot` in portrait. Pointer maths goes through `P(e)` and `R(el)`, so any new touch code must use them.
- Layouts use the same model as the Android app: 13 controls, anchors, groups, and offsets in multiples of a base unit. Layouts are shared as JSON, see [layouts](https://github.com/horse-3903/AirDeck/blob/main/docs/layouts.md#sharing-text).
- Settings are rows from a `SPECS` table, stored in `localStorage` under `vgp-<key>`, with defaults in `SETTING_DEFAULTS`. A layout can override them through its `behaviour` map. The effective value is the layout override, else the global setting.
- Response curves are up to 10 points joined by a monotone cubic or linear segments, applied to sticks, tilt and triggers.
- The server stamps the page version into `__VGP_V__`, which lets open pages reload after an upgrade.

## Known limits

- Windows can number the only virtual pad 2 or 3 even with nothing else present. A reboot may fix it.
- iPhone only supports basic timed vibration pulses and no fullscreen in a tab.
- Slots 5 to 8 may be invisible to XInput-only games.
- Elevated games ignore input from a non-elevated AirDeck.
