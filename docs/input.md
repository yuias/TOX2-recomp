# Input

The pad on port 0 of the game's `cellPad` comes from the keyboard, from a gamepad, from both
together, or from a pad script. This page covers gamepads: what a player needs, how the code is
organised, how pads are selected, the log lines, the diagnostic option `--pad-probe` and the tool
`dal_padtool`. The keyboard, the pad script and the `PadProvider` seam are in
[Input](runtime.md#input) in the runtime internals, and the option defaults are in
[`dal_game` options](runtime-options.md#window-input-and-sound).

## Gamepads

A run with the window reads every connected gamepad together with the keyboard. All pads and the
keyboard drive port 0 at the same time; ports 1 to 6 stay empty, because the game is single-player.
Runs with `--no-window` never load GameInput, so unattended runs and the tests do not depend on a
pad or on the GameInput runtime.

### Supported pads

- **Verified:** an Xbox Wireless Controller over Bluetooth LE. GameInput lists it as
  `gamepad+controller` with rumble, and buttons, d-pad, both sticks, both triggers and vibration were
  checked with it. Other Xbox-family pads should behave the same if GameInput reports them as
  gamepads; they were not tried.
- **Verified:** a DualShock 4 (second model, `054c:09cc`) over Bluetooth. GameInput lists it as
  `gamepad+controller` without rumble and maps it by position: Cross, Circle, Square and Triangle
  read as A, B, X and Y; Share and Options as View and Menu; L1/R1, L2/R2, L3/R3, the d-pad and both
  sticks as on the Xbox pad (full range on every axis). The PS button and the touch pad have no
  gamepad button and are not read. In the game the pad is selected and the title menu works with it.
  No vibration: GameInput reports no rumble for it.
- **DualShock 4, first model (`054c:05c4`): not supported.** It was seen once over Bluetooth as a
  generic controller without the gamepad kind, so the game logs it as `not a gamepad` and ignores it.
- While Steam runs with its PlayStation controller support on, Steam takes the DualShock 4 and no
  input from it reaches the game or `dal_padtool`. Quit Steam or turn that setting off.
- Other pads are used only if GameInput reports them with the gamepad kind. A device without it is
  logged as `not a gamepad` and ignored.

### Mapping

The mapping is positional and fixed; there is no remapping or configuration file.

| Gamepad | PS3 pad |
| --- | --- |
| A / B / X / Y | Cross / Circle / Square / Triangle |
| Left bumper / right bumper | L1 / R1 |
| Left trigger / right trigger | L2 / R2 (analog) |
| View / Menu | Select / Start |
| D-pad | D-pad |
| Left stick click / right stick click | L3 / R3 |

The game confirms with Circle, which is the B button. The guide button and the paddles are not
used. Motion sensors, the touchpad and the light bar of other pads are not supported either; the
sensor words of the PS3 pad stay at rest.

### Sticks and triggers

- Each stick has a radial dead zone that is rescaled, so there is no jump at its edge: 7849/32767
  (about 0.24) for the left stick and 8689/32767 (about 0.265) for the right stick, the values of
  XInput. A stick inside its dead zone reports the centre.
- A trigger counts as pressed (L2 or R2) from 30/255 of its travel. Below that the button is
  released and its pressure is 0; above it the game receives the analog value as the pressure of
  L2 or R2. All other pressed buttons report full pressure. A trigger that is also held on the
  keyboard reports full pressure.
- With several pads, buttons are ORed, each stick axis takes the value farthest from the centre
  (the first pad on a tie) and each trigger takes the larger value.
- An idle pad reports exactly the neutral state, so it causes no input.

### Vibration

The game's vibration requests (`cellPadSetActDirect`) are forwarded to every used pad that has
rumble: the large motor sets the low-frequency motor in proportion, and the small motor, which is
on or off on a PS3 pad, sets the high-frequency motor to full strength. Trigger motors are not used.
`dal_padtool rumble` checks a pad's motors without the game. Whether and when the game asks for
vibration was not checked in play.

### Focus

GameInput delivers readings to the foreground window only, and the game additionally checks that
its own process owns the foreground window. While another window has the focus, the pad's buttons are
released at once, the sticks and triggers go back to neutral and vibration stops; the keyboard's
buttons are released separately when the window loses focus. Input resumes when the window is in
front again.

One case needs care: a d-pad direction that is held while switching away must be released and
pressed again after returning. GameInput's foreground-only reading stays neutral until the state
changes.

### The GameInput runtime and the `gameinput unavailable` warning

Gamepads are read with Microsoft's GameInput (API version 3). The build links only the package's
static loader, so `dal_game` starts on a machine without a GameInput runtime and the keyboard keeps
working. The runtime itself must be installed once, from `GameInputRedist.msi` in the fetched
package (the step is in the [README](../README.md#building)).

If no usable runtime is found, the game logs one warning and runs without gamepads:

```
dal: pad: gameinput unavailable: <reason>; gamepads disabled
```

`<reason>` names the failing step and its error code and, for a missing runtime or one without
API version 3, says to install `GameInputRedist.msi`. Installing the MSI is the fix. A copy of the
redistributable DLL next to `dal_game.exe` is not used: it takes precedence over the installed
runtime and hides Xbox pads.

With `--pad gamepad` (no keyboard) and no runtime, port 0 stays disconnected.

### Steam

Started from Steam as a non-Steam game, the Steam overlay hooks GameInput inside the process; the
log shows `dal: pad: gameinput v3 from gameoverlayrenderer64.dll`. To set it up:

1. In Steam, add `dal_game.exe` with Games > Add a Non-Steam Game.
2. In the shortcut's properties set Target to `<repository>\build\release\bin\dal_game.exe` and Start
   In to `<repository>`, the repository root. The game resolves `generated/shader_cache` and relative
   option paths against the start directory.
3. Put the options into Launch Options with full paths, for example `--game-dir "<game-dir>"`. Steam
   does not expand environment variables there. Add `--log <repository>\build\release\pad\steam.log
   --log-level debug` to keep a log.
4. Leave "Enable the Steam Overlay while in-game" on.
5. Under Properties > Controller, the per-game setting turns Steam Input on or off for this game.

What was observed with an Xbox Wireless Controller:

- With Steam Input enabled and with it disabled, the game saw the same single physical pad and read
  it directly. No separate virtual pad appeared, and the Steam environment variables were the same in
  both cases.
- Whether Steam Input remaps the buttons the game reads (binding A to Y and pressing A) was not
  verified. Do not rely on Steam's controller configuration to remap.
- The DualShock 4 was not tried through a Steam shortcut; see Supported pads for Steam running
  in the background.

The selection code (below) already handles a Steam virtual pad should one appear: it reads the virtual
pad and ignores the physical pad behind it. That path is covered by unit tests with a path format
taken from SDL's reading of Steam's device paths, not by a real Steam run.

## Design

### Where the code lives

| Part | File | Library | Role |
| --- | --- | --- | --- |
| Backend interface | `runtime/src/input/gamepad_backend.h` | `dal_runtime_core` | `GamepadReading` (buttons, sticks -1 to 1 with +y up, triggers 0 to 1), `GamepadDeviceInfo`, `SteamContext`, and `GamepadBackend` (the device list with a generation counter, `read`, `setRumble`); no Windows or GameInput types |
| GameInput backend | `runtime/src/input/gameinput_backend.*` | `dal_input_gameinput` | The only code that includes `GameInput.h` and `<windows.h>` for gamepads: loading, the device table, readings, rumble, the focus policy, `detectSteamContext` and `processOwnsForegroundWindow`; also `readRaw`, the generic controller layout used by `dal_padtool watch --raw` |
| Mapping | `runtime/src/input/gamepad_map.*` | `dal_runtime_core` | Button table, dead zone, trigger threshold, merging of several pads, motor conversion |
| Selection | `runtime/src/input/gamepad_select.*` | `dal_runtime_core` | Recognising Steam's virtual pad and the rule that picks the pads in use |
| Hub | `runtime/src/input/gamepad_hub.*` | `dal_runtime_core` | The gamepad thread: devices, selection, readings, focus gate and rumble, written to the shared state |
| Probe and formatters | `runtime/src/input/gamepad_probe.*` | `dal_runtime_core` | `--pad-probe`, and the text formats that the log and `dal_padtool` share |
| Shared state | `runtime/src/input/keyboard_pad.*`, `pad_buttons.*` | `dal_runtime_core` | The `Gamepad` input source, the analog snapshot, the connection flag and the stored vibration |
| Targets | `cmake/Input.cmake`, `cmake/gameinput.cmake`, `tools/fetch-gameinput.ps1` | | The package check, `dal_input_gameinput`, `dal_padtool`, the tests, and the pinned package |

Mapping, selection, the hub and the probe are core code behind `GamepadBackend`, so `input_core`
tests them with a fake backend and no GameInput.

### The GameInput package

`tools/fetch-gameinput.ps1` downloads the pinned `Microsoft.GameInput` NuGet package
(`cmake/gameinput.cmake`: version and SHA-256), checks the hash and extracts it to
`generated/deps/gameinput/<version>`. Configure stops with the fetch command when the package is
missing or does not match the pin. The build uses the package's `GameInput.h` and `GameInput.lib`.

- **Header.** The Windows SDK ships an older `GameInput.h` on the default include path.
  `dal_input_gameinput` adds the package's directory as a `SYSTEM` include, which is searched first,
  and `gameinput_backend.cpp` asserts `GAMEINPUT_API_VERSION == 3`.
- **Library.** The package's `GameInput.lib` is a static loader that resolves the runtime with
  `LoadLibrary` at the first call, so no executable has a GameInput DLL in its import table. The
  SDK's own `gameinput.lib` is an import library for the in-box runtime and is never linked.
- **Runtime.** Nothing from the package is copied into the build output; the installed runtime is
  used (see [above](#the-gameinput-runtime-and-the-gameinput-unavailable-warning)).

### Threads and the guest lock

`PadProvider::poll` and `setVibration` run on guest threads under the guest lock, so they never
block, never call GameInput and never take a lock that another thread holds. The Gamepad source of
`KeyboardPadState` is therefore lock-free like the keyboard and script sources: atomic button masks
and press times, the sticks and trigger pressure packed into one atomic, the connection flag, and the
latest vibration as one atomic that the gamepad thread forwards. `poll` only reads them.

The gamepad thread (`startGamepadHub`) is a detached host thread that owns its `shared_ptr`s;
nothing joins it, because the process ends with `std::_Exit`. It:

1. creates the GameInput backend, which takes about a second and so stays off the boot path;
2. logs the start lines (below);
3. runs the hub every 4 ms (`kGamepadPollInterval`), and, with `--pad-probe`, the probe every 50 ms
   (`kGamepadProbeInterval`), always from the same thread, because the backend allows one calling
   thread.

The thread is the only writer of the Gamepad source. The window's focus-lost handler releases the
Keyboard source only.

Each hub step:

1. When the backend's device generation changed, lists the devices, applies the selection rule, logs
   every device whose use changed and sets the connection flag (at least one pad in use).
2. Applies the focus gate: on the change to the background it releases the Gamepad buttons without the
   minimum visible time, resets the analog snapshot and stops the motors; while in the background it
   does nothing else. The first step only records the state, because the window is created later.
3. Reads every pad in use, maps and merges the readings and stores buttons and analog values when
   they changed. A pad without a reading contributes nothing.
4. Forwards the vibration when it changed or the set of pads in use changed.

The 60 ms minimum visible time and the opposite-direction rule for the d-pad apply to the gamepad
source like to the keyboard (see [Key handling](runtime.md#key-handling)).

Device enumeration is not complete when the backend is created: the runtime reports a pad some 30
to 50 ms after the blocking enumeration returns. Consumers therefore follow the generation counter
instead of assuming a complete list, and `device +` lines can appear before the `gameinput` line.

### Device selection

`selectGamepads` gives every listed device one use:

1. A device without the gamepad kind is `not a gamepad`.
2. A device whose `pnpPath` parses as Steam's virtual pad (an Xbox 360 pad, `\\.\pipe\HID#VID_045E&PID_028E&IG_...`
   with the real vendor and product id, slot and process id) is `used`, or `other process` when the
   path names a different process.
3. A physical device is `handled by steam` when its vendor and product id equal those behind a
   virtual pad that is present, or, with the overlay loaded, when they appear in
   `SDL_GAMECONTROLLER_IGNORE_DEVICES` (comma-separated `0xVVVV/0xPPPP`).
4. Otherwise it is `used`.

Today only rules 1 and 4 apply on a real system: in the Steam runs, the overlay was loaded, no
virtual pad was listed, and `SDL_GAMECONTROLLER_IGNORE_DEVICES` never named the Xbox pad, with Steam
Input on or off. Two identical physical pads of which Steam handles only one are both dropped by
rule 3; the game is single-player and this is accepted.

## Log lines

All gamepad lines start with `dal: pad:`. The start line of the pad itself is
`dal: pad: <pad>[ (window)][ (script <file>, <n> events)]`, for example
`dal: pad: keyboard+gamepad (window)`.

| Line | Level | Source |
| --- | --- | --- |
| `dal: pad: gameinput v3 from <module>` | info | after the backend is created; the module is the file name that provides GameInput (`gameoverlayrenderer64.dll` under Steam's hook) |
| `dal: pad: gameinput unavailable: <reason>; gamepads disabled` | warn | the backend could not be created; no hub and no probe |
| `dal: pad: steam overlay loaded\|not loaded[; <NAME>=<value> ...]` | info | right after the backend is created; lists the Steam variables that are set (`SteamGameId`, `SteamOverlayGameId`, `SteamAppId`, `EnableConfiguratorSupport`, `SteamVirtualGamepadInfo`, `SDL_GAMECONTROLLER_IGNORE_DEVICES`, `SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD`); `SteamVirtualGamepadInfo` prints as `set` |
| `dal: pad: device + <vid:pid> <kinds> rumble yes\|no "<name>"` | info | a device arrived; `<kinds>` is `gamepad`, `controller` or `gamepad+controller` |
| `dal: pad: device <id> path <pnp path>` | debug | the device's path, with the arrival |
| `dal: pad: device - <vid:pid>` | info | a device was removed |
| `dal: pad: gamepad <vid:pid> <use>[ (steam slot <n>)]` | info | a device's use changed; `<use>` is `used`, `handled by steam`, `other process` or `not a gamepad` |
| `dal: pad: gamepad input paused (background)` and `dal: pad: gamepad input resumed` | debug | the process lost or regained the foreground window |
| `dal: pad: probe ...` | info | only with `--pad-probe` (below) |

`cellPad` itself logs `dal: pad: port 0 connected` and `disconnected`. With `--pad gamepad` they
follow the pad being plugged in and out; otherwise port 0 is always connected, because the keyboard is
part of the pad.

## `--pad-probe`

`--pad-probe` is a diagnostic for bug reports and for checking what Windows and Steam present. It
needs the window, is independent of `--pad`, and does not feed the pad: with `--pad none`, `neutral`
or `keyboard` it only logs. Next to the start lines it logs:

| Line | Meaning |
| --- | --- |
| `dal: pad: probe <id> <vid:pid> buttons <names> lt <f> rt <f> ls (<x>,<y>) rs (<x>,<y>)` | A reading of any listed device, used by the game or not, whenever the text changes. `<names>` joins the pressed buttons with `+` or is `none`; sticks and triggers are rounded to steps of 0.05 so noise does not produce lines |
| `dal: pad: probe <id> <vid:pid> no gamepad reading` | Once per device that has no gamepad reading, such as a generic controller |
| `dal: pad: probe SteamVirtualGamepadInfo unset` (or `unreadable`, `empty`) | Whether the variable of that name is set, and if it names a readable file, one `dal: pad: probe SteamVirtualGamepadInfo: <line>` per line of its first 4 KiB |

Probe readings follow GameInput's foreground-only policy, so they can be neutral or stale while the
window is in the background. Add `--log-level debug --log FILE` to keep the device paths in a file.

## `dal_padtool`

`build/release/bin/dal_padtool.exe` uses the same backend without the game. It asks GameInput for
background input, because its console window belongs to another process. That works for the Xbox pad
but not for the DualShock 4, whose input arrives only while the console window is in the foreground. Output goes to stdout. Exit
codes: 0 on success, 1 when GameInput is unavailable (the message names the reason, on stderr), 2 on
a usage error.

| Subcommand | Output |
| --- | --- |
| `devices` | `gameinput v3 from <module>`, `steam overlay loaded\|not loaded`, one `steam <NAME>=<value>` per set variable, one line per device, `<id> <vid:pid> <kinds> rumble yes\|no "<name>" <pnp path>`, and `<n> devices`. It waits until the device list has been quiet for 250 ms (at most 2 s) |
| `watch [--seconds N] [--raw]` | Polls every device every 10 ms for N seconds (default 10) and prints a line per change: `<t> s <id> <vid:pid> buttons <names> lt <f> rt <f> ls (<x>,<y>) rs (<x>,<y>)` with the values rounded to 0.05, and `<t> s device + ...` or `device - ...` when a device arrives or leaves. `--raw` also prints the generic controller layout of controller-kind devices: `raw buttons [<pressed indices>] axes [<index>:<value> ...] switches [<index>:<position> ...]`, where a switch position is 0 for centre, 1 for up and then clockwise to 8 |
| `rumble [--seconds N]` | For each device with rumble: `low frequency motor` for N seconds (default 1), `high frequency motor` for N seconds, then `stopped`; finally `<n> devices with rumble` |

## Licence

The GameInput header and static library are MIT-licensed. The redistributable runtime is under
Microsoft's own terms and is neither copied nor redistributed by this project; the user installs it.
See `THIRD_PARTY_NOTICES.md`.
