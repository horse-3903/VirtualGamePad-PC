# Roadmap

Planned work for AirDeck-PC, roughly in the order it will be built. Nothing here is a promise, and the order changes as things are tried.

## Done

- Game rumble from ViGEm to the phone and the browser (PC side logged, end to end with a game still to confirm)
- Layout library, import and export, and layout switching (Android app)
- Clean controller slate when the server starts or restarts
- Bounded debug log
- Joy-Con button style in the browser controller
- Browser layout editor with the app's model: anchors, groups, snapping, undo, shared layout format
- Choose Xbox 360 or PlayStation (DualShock 4) as the virtual controller type
- Round-trip latency readout on each controller card, with a summary in the debug log
- Optional approve prompt before a new device can connect, with remembered devices

## Next

- **Short pairing code** - a code shown on the PC and typed on the phone, for a stronger check than a remembered address.
- **Latency history** - a graph of round trip time over a session, and published measurements for latency and jitter. A test script already exists in the AirDeck repo under `tools/latency_test.py`.

## Later

- **Automatic game profiles** - read Steam's `RunningAppID` registry value and load the layout saved for that game, with the foreground process name as a fallback for non-Steam games.
- **Split controller** - two phones feeding one virtual controller, for co-op on a single player's pad or steering and pedals on separate devices.
- **Haptic pattern editor** - custom vibration patterns that can be shared.
- **Game telemetry for adaptive layouts** - read game state so the phone can switch layout by context, such as on foot or driving.
- **Event stream for the haptic engine** - send telemetry and rumble events to the phone so it can mix them with its own cues.
- **Device fusion** - let several devices form one logical controller, an extension of the split controller.
- **Evaluation harness** - repeatable tests for missed presses, tracking error and latency, to compare layouts and input methods.
- **Layout sharing** - a place to publish and download layouts.

The wider plan is in the [AirDeck roadmap](https://github.com/horse-3903/AirDeck/blob/main/ROADMAP.md).

## Known limits

- Controllers 5 to 8 appear as DualShock 4 style pads, so games that only read XInput will not see them.
- There is no encryption or login, so use it on networks you trust.
