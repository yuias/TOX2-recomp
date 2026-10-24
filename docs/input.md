# Input

The pad on port 0 of the game's `cellPad` comes from the keyboard, from a gamepad, from both
together, or from a pad script. This page covers gamepads: what a player needs, how the code is
organised, how pads are selected, how a system dialog takes the pad from the game, the log lines, the diagnostic
option `--pad-probe` and the tool `tox2_padtool`. The sections about GameInput, Steam and `tox2_padtool` describe Windows;
[Gamepads on macOS](#gamepads-on-macos) describes the macOS backend. The keyboard, the pad script and
the `PadProvider` seam are in [Input](runtime.md#input) in the runtime internals, and the option defaults are in
[`tox2_game` options](runtime-options.md#window-input-and-sound).

*Tales of Xillia 2* imports six `cellPad` functions (`cellPadInit`, `cellPadEnd`,
`cellPadSetPortSetting`, `cellPadGetData`, `cellPadGetInfo2` and `cellPadSetActDirect`), and the
runtime has a handler for each. The game calls `cellPadInit` with 7 ports, sets press and sensor mode (setting
`0x6`) on all seven, and then calls `cellPadGetData`, `cellPadGetInfo2` and `cellPadSetActDirect` about once per
frame (the three counts are equal in the census; the 30 s headless run in [Status](status.md) has 7,794
`cellPadGetData` calls). Input from the keyboard and from pad scripts reaches the game, and a pad script has taken it
through the title screen and its first choices. The game has been played with a gamepad. A pad connected
after the start is not picked up by the game; while Steam runs, Steam handles the pad and
reconnection. The finer statements below about pad models, buttons and Steam come from `tox2_padtool`, the tests
and runs of other builds, and have not been repeated one by one in this game.

## Gamepads

A run with the window reads every connected gamepad together with the keyboard. All pads and the
keyboard drive port 0 at the same time; ports 1 to 6 stay empty, because the runtime provides one pad.
Runs with `--no-window` never load GameInput (Windows) or GameController.framework (macOS), so
unattended runs and the tests do not depend on a pad or on the GameInput runtime.

### Supported pads

- **Checked with `tox2_padtool`:** an Xbox Wireless Controller over Bluetooth LE. GameInput lists it as
  `gamepad+controller` with rumble, and buttons, d-pad, both sticks, both triggers and vibration were
  checked with it; they have not been checked in this game. Other Xbox-family pads should behave the same if GameInput reports them as
  gamepads; they were not tried.
- **Checked with `tox2_padtool`:** a DualShock 4 (second model, `054c:09cc`) over Bluetooth. GameInput lists it as
  `gamepad+controller` without rumble and maps it by position: Cross, Circle, Square and Triangle
  read as A, B, X and Y; Share and Options as View and Menu; L1/R1, L2/R2, L3/R3, the d-pad and both
  sticks as on the Xbox pad (full range on every axis). The PS button and the touch pad have no
  gamepad button and are not read. The runtime selects the pad; its use in this game's menus has not been checked. No vibration: GameInput reports no rumble for it.
- **DualShock 4, first model (`054c:05c4`): not supported.** It was seen once over Bluetooth as a
  generic controller without the gamepad kind, so the game logs it as `not a gamepad` and ignores it.
- While Steam runs with its PlayStation controller support on, Steam takes the DualShock 4 and no
  input from it reaches the game or `tox2_padtool`. Quit Steam or turn that setting off.
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

The guide button and the paddles are not used. Motion sensors, the touchpad and the light bar of other pads are not supported either; the
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
`tox2_padtool rumble` checks a pad's motors without the game. This game calls `cellPadSetActDirect`
about once per frame; which motor values it sends, and whether a pad then vibrates in the game, has not been checked.

### Focus

GameInput delivers readings to the foreground window only, and the runtime additionally checks that
its own process owns the foreground window. While another window has the focus, the pad's buttons are
released at once, the sticks and triggers go back to neutral and vibration stops; the keyboard's
buttons are released separately when the window loses focus. Input resumes when the window is in
front again.

One case needs care: a d-pad direction that is held while switching away must be released and
pressed again after returning. GameInput's foreground-only reading stays neutral until the state
changes.

### The GameInput runtime and the `gameinput unavailable` warning

Gamepads are read with Microsoft's GameInput (API version 3). The build links only the package's
static loader, so `tox2_game` starts on a machine without a GameInput runtime and the keyboard keeps
working. The runtime itself must be installed once, from `GameInputRedist.msi` in the fetched
package (the step is in the [README](../README.md#building)).

If no usable runtime is found, the game logs one warning and runs without gamepads:

```
tox2: pad: gameinput unavailable: <reason>; gamepads disabled
```

`<reason>` names the failing step and its error code and, for a missing runtime or one without
API version 3, says to install `GameInputRedist.msi`. Installing the MSI is the fix. A copy of the
redistributable DLL next to `tox2_game.exe` is not used: it takes precedence over the installed
runtime and hides Xbox pads.

With `--pad gamepad` (no keyboard) and no runtime, port 0 stays disconnected.

### Steam

Started from Steam as a non-Steam game, the Steam overlay hooks GameInput inside the process; the
log shows `tox2: pad: gameinput v3 from gameoverlayrenderer64.dll`. To set it up:

1. In Steam, add `tox2_game.exe` with Games > Add a Non-Steam Game.
2. In the shortcut's properties set Target to `<repository>\build\release\bin\tox2_game.exe` and Start
   In to `<repository>`, the repository root. The game resolves `generated/shader_cache` and relative
   option paths against the start directory.
3. Put the options into Launch Options with full paths, for example `--game-dir "<game-dir>"`. Steam
   does not expand environment variables there. Add `--log <repository>\build\release\pad\steam.log
   --log-level debug` to keep a log.
4. Leave "Enable the Steam Overlay while in-game" on.
5. Under Properties > Controller, the per-game setting turns Steam Input on or off for this game.

What was observed with an Xbox Wireless Controller, running a build of the runtime made for a different game
(not repeated with this game):

- With Steam Input enabled and with it disabled, the process saw the same single physical pad and
  read it directly. No separate virtual pad appeared, and the Steam environment variables were the same in
  both cases.
- Whether Steam Input remaps the buttons the pad provides (binding A to Y and pressing A) was not
  verified. Do not rely on Steam's controller configuration to remap.
- The DualShock 4 was not tried through a Steam shortcut; see Supported pads for Steam running
  in the background.

The selection code (below) already handles a Steam virtual pad should one appear: it reads the virtual
pad and ignores the physical pad behind it. That path is covered by unit tests with a path format
taken from SDL's reading of Steam's device paths, not by a real Steam run.

## Gamepads on macOS

On macOS the gamepad backend is `tox2_input_gamecontroller` (`gamecontroller_backend.mm`, Objective-C++),
which reads the controllers that GameController.framework reports. Nothing has to be installed. The
mapping, the dead zones, the trigger threshold, the merging of several pads, the focus gate and the
vibration conversion described above are core code behind the backend interface and work the same;
this section lists what differs. It was written from the code: on the Mac where the port was run, `input_gamecontroller`
skipped its device part because no controller was connected, so no gamepad has been checked on macOS.

- **Pads.** A controller whose profile includes `extendedGamepad` has the gamepad kind and is read.
  Any other controller is listed with the controller kind only; the hub logs it as `not a gamepad`
  and ignores it. Which controllers macOS gives that profile is up to the system.
- **No vendor or product id, no path.** The framework does not give them, so every device reports
  `0000:0000` in the log and in `--pad-probe`, its path is empty and its name is the controller's
  `vendorName`. Device ids increase with every arrival and are never reused, and the device
  generation grows with every connect and disconnect. The `device +`, `device -` and `device <id> path`
  lines are written by the GameInput backend only, so a run on macOS shows the hub's
  `gamepad 0000:0000 used` lines but no `device +` lines.
- **Buttons.** The framework's A, B, X and Y become the face buttons of the table above, the left and
  right shoulders L1 and R1, `buttonMenu` Start and `buttonOptions` Select, then the stick clicks, the
  d-pad, the sticks (with +y up) and the analog triggers. A pad without an options button simply never
  presses Select.
- **Steam.** Only the environment variables of the Steam line are read (`tox2: pad: steam overlay not
  loaded[; ...]`); the overlay is never reported as loaded. Without a device path no Steam virtual pad
  can be recognised, and the `SDL_GAMECONTROLLER_IGNORE_DEVICES` rule needs the overlay, so every
  controller with the gamepad kind is used.
- **Focus.** The gamepad hub counts the process as being in front while it is the active application
  (`NSRunningApplication`). While another application is active the buttons are released and the
  motors stop, as described under Focus above.
- **Threads.** Controller connections and disconnections are reported on the main queue, which
  `tox2_game` serves with its event loop on the main thread (see
  [Building](building.md#running-and-debugging-on-a-mac)); the backend needs the main run loop to
  learn about controllers. Each controller delivers its state on a private serial queue into a copy that
  `read` returns under a mutex, so the gamepad thread never calls the framework while a handler runs.
  Controllers that are connected when the backend is created are listed at once.
- **Vibration.** Rumble goes through Core Haptics. A controller has rumble when the framework gives it
  a haptics object. For each such controller the backend keeps two haptic engines, created when the
  motor is first set to a non-zero strength. When the controller offers the left and the right handle
  as haptic localities, the low-frequency motor drives the left handle and the high-frequency motor
  the right one. Otherwise both use the default locality and differ only in sharpness (smooth for
  the low-frequency motor, sharp for the high-frequency one). Each motor plays a continuous event of
  30 s in a loop, and the game's strength is the event's intensity, changed while it plays; a strength
  of zero stops it. An engine that the system stops or resets is created again at the next request.
  A step that fails is logged once per motor as `tox2: pad: haptics: <step> failed: <reason>`, and the
  game goes on. Trigger motors are not used.
- **Log.** The start line is `tox2: pad: gameinput GameController.framework`: the prefix is shared
  with Windows and the rest is the backend's description. If the framework is missing, the warning
  `tox2: pad: gameinput unavailable: GameController.framework is not available; gamepads disabled`
  appears and the keyboard keeps working.
- **No `tox2_padtool`.** The tool is Windows only; `--pad-probe` reports devices and readings on macOS.
  `input_gamecontroller` tests the backend with a real controller (see [Tests](tests.md)).

### Keyboard and the overlay on macOS

The keyboard is the same pad source as on Windows. The Cocoa window turns each key event into a PC/AT
scan code from the key's macOS virtual key code (`runtime/src/input/mac_key_codes.*`), which is a
physical position, so AZERTY and QWERTZ keyboards keep the layout of the README table. The modifier
keys come from the flags-changed events, left and right apart. Keys that have no scan code (`Fn`,
media keys, the Japanese `Eisu` and `Kana` keys) are not used.

- **Cmd.** A key pressed together with `Cmd` is a system shortcut: AppKit takes the key-down
  (`Cmd+Q` ends the run), and the pad never sees it. The key-up of such a key is still given to the
  window, so a key pressed with `Cmd` does not stay down for the pad. The `Cmd` keys themselves are
  not pad buttons.
- **Other keys** are consumed by the window and not passed on, so macOS does not beep. The window has
  no input method: key events never reach the text input system.
- **Overlay.** The debug overlay takes the mouse and keys through Dear ImGui's macOS backend
  (`imgui_impl_osx`), reached through the bridge `runtime/src/overlay/imgui_osx_bridge.mm`, which
  compiles the backend source and lets the window pass events to it under the overlay's mutex instead
  of the backend's own event monitor. `F1` and `Tab` work as on Windows. Typed text for the overlay's
  text boxes is the key's characters, without an input method or the clipboard.

## System dialogs

The save data list dialog (see [Save data dialog](runtime.md#save-data-dialog)) is operated with the same pad that
plays the game, so it takes the pad for as long as it is open.

- **Interception.** `setPadIntercepted(true)` in `runtime/src/hle/cell_pad.cpp` makes `cellPadGetData` write a length
  of 0 (no change) and leave the data and the game's last reported state alone, and `cellPadGetInfo2` set bit `0x1` of
  `system_info` (`CELL_PAD_INFO_INTERCEPTED`), as the firmware does while a system dialog has the pad. The dialog reads
  the pad itself with `pollHostPad`, which returns the provider's state without change detection and without touching
  what the game was told.
- **Release phase.** When the dialog closes, `setPadIntercepted(false)` starts a release phase: the game sees one
  change to a neutral pad (all buttons up, sticks centred), then no change, until no START, SELECT, cross, circle,
  triangle or square is held on any connected port. The press that closed the dialog therefore does not also act in
  the game's menu. After that the next `cellPadGetData` reports the real state as a change. `resetCellPad` clears
  both flags.
- **Enter and back.** Circle is enter and cross is back, as on a Japanese console. The assignment is fixed for now: it
  is `dialogButtons()` in `runtime/src/input/pad_buttons.*`, set by `setEnterButtonAssign` (default circle), and the
  model and the legend both read it, but no option or menu changes it. With the mappings above that is the B and A
  buttons of an Xbox pad and the `X` and `Z` keys.
- **Dialog input.** Up and down come from the d-pad and from the vertical axis of either stick (below 30 is up, above 225
  is down, of 0 to 255); they repeat after 500 ms and then every 200 ms. L1 and R1 move ten rows, enter and back act
  once per press. Every input counts as held when the dialog opens, so a button still down from the game acts only
  after it was released and pressed again; other buttons do nothing, and input is ignored during the 150 ms fade-in
  and fade-out. The dialog reads the pad provider only: the mouse and the other keys do nothing, and `F1` still
  shows the debug overlay, drawn above the dialog.
- **When it is used.** The dialog needs a pad that reads host input (`keyboard`, `gamepad` or `keyboard+gamepad`)
  and no `--pad-script`; with `--pad none` or `neutral` it is answered automatically (see `--save-dialog` in
  [`tox2_game` options](runtime-options.md#window-input-and-sound)).

## Design

### Where the code lives

| Part | File | Library | Role |
| --- | --- | --- | --- |
| Backend interface | `runtime/src/input/gamepad_backend.h` | `tox2_runtime_core` | `GamepadReading` (buttons, sticks -1 to 1 with +y up, triggers 0 to 1), `GamepadDeviceInfo`, `SteamContext`, and `GamepadBackend` (the device list with a generation counter, `read`, `setRumble`); no Windows or GameInput types |
| GameInput backend (Windows) | `runtime/src/input/gameinput_backend.*` | `tox2_input_gameinput` | The only code that includes `GameInput.h` and `<windows.h>` for gamepads: loading, the device table, readings, rumble, the focus policy, `detectSteamContext` and `processOwnsForegroundWindow`; also `readRaw`, the generic controller layout used by `tox2_padtool watch --raw` |
| GameController backend (macOS) | `runtime/src/input/gamecontroller_backend.*` | `tox2_input_gamecontroller` | The only code that includes the GameController, Core Haptics and AppKit headers: the device table from the connect and disconnect notifications, readings copied from the controllers' handlers, rumble through Core Haptics, `detectSteamContext` (environment variables only) and `processOwnsForegroundWindow` |
| Key codes | `runtime/src/input/mac_key_codes.*` | `tox2_runtime_core` | Plain tables: macOS virtual key code to PC/AT scan code, and the modifier bit of each modifier key; built and tested on every system |
| Mapping | `runtime/src/input/gamepad_map.*` | `tox2_runtime_core` | Button table, dead zone, trigger threshold, merging of several pads, motor conversion |
| Selection | `runtime/src/input/gamepad_select.*` | `tox2_runtime_core` | Recognising Steam's virtual pad and the rule that picks the pads in use |
| Hub | `runtime/src/input/gamepad_hub.*` | `tox2_runtime_core` | The gamepad thread: devices, selection, readings, focus gate and rumble, written to the shared state |
| Probe and formatters | `runtime/src/input/gamepad_probe.*` | `tox2_runtime_core` | `--pad-probe`, and the text formats that the log and `tox2_padtool` share |
| Shared state | `runtime/src/input/keyboard_pad.*`, `pad_buttons.*` | `tox2_runtime_core` | The `Gamepad` input source, the analog snapshot, the connection flag and the stored vibration; `pad_buttons.*` also holds `dialogButtons()`, the enter and back buttons of system dialogs |
| Targets | `cmake/Input.cmake`, `cmake/gameinput.cmake`, `tools/fetch-gameinput.ps1` | | The package check, `tox2_input_gameinput`, `tox2_padtool` and `input_gameinput` (Windows), `tox2_input_gamecontroller` and `input_gamecontroller` (macOS), `input_core`, and the pinned package |

Mapping, selection, the hub and the probe are core code behind `GamepadBackend`, so `input_core`
tests them with a fake backend and no GameInput or GameController.

### The GameInput package

`tools/fetch-gameinput.ps1` downloads the pinned `Microsoft.GameInput` NuGet package
(`cmake/gameinput.cmake`: version and SHA-256), checks the hash and extracts it to
`generated/deps/gameinput/<version>`. Configure stops with the fetch command when the package is
missing or does not match the pin. The build uses the package's `GameInput.h` and `GameInput.lib`.

- **Header.** The Windows SDK ships an older `GameInput.h` on the default include path.
  `tox2_input_gameinput` adds the package's directory as a `SYSTEM` include, which is searched first,
  and `gameinput_backend.cpp` asserts `GAMEINPUT_API_VERSION == 3`.
- **Library.** The package's `GameInput.lib` is a static loader that resolves the runtime with
  `LoadLibrary` at the first call, so no executable has a GameInput DLL in its import table. The
  SDK's own `gameinput.lib` is an import library for the in-box runtime and is never linked.
- **Runtime.** Nothing from the package is copied into the build output; the installed runtime is
  used (see [above](#the-gameinput-runtime-and-the-gameinput-unavailable-warning)).

### Threads and the guest lock

`PadProvider::poll` and `setVibration` run on guest threads under the guest lock (the kernel lock in `--ppu-threads 1|2`, see [Threads](runtime.md#threads)), so they never
block, never call GameInput and never take a lock that another thread holds. The Gamepad source of
`KeyboardPadState` is therefore lock-free like the keyboard and script sources: atomic button masks
and press times, the sticks and trigger pressure packed into one atomic, the connection flag, and the
latest vibration as one atomic that the gamepad thread forwards. `poll` only reads them.

The save data dialog reads the pad the same way: the waiting guest thread calls `pollHostPad` after each timed wait,
with the lock held again, never from the RSX thread.

The gamepad thread (`startGamepadHub`) is a detached host thread that owns its `shared_ptr`s;
nothing joins it, because the process ends with `std::_Exit`. It:

1. creates the gamepad backend (GameInput or GameController), which can take about a second and so stays off the boot path;
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
rule 3; the runtime provides one pad and this is accepted.

## Log lines

All gamepad lines start with `tox2: pad:`. The start line of the pad itself is
`tox2: pad: <pad>[ (window)][ (script <file>, <n> events)]`, for example
`tox2: pad: keyboard+gamepad (window)`.

| Line | Level | Source |
| --- | --- | --- |
| `tox2: pad: gameinput GameController.framework` | info | macOS: after the backend is created. The `gameinput` prefix is shared with the Windows line below |
| `tox2: pad: gameinput v3 from <module>` | info | after the backend is created; the module is the file name that provides GameInput (`gameoverlayrenderer64.dll` under Steam's hook) |
| `tox2: pad: gameinput unavailable: <reason>; gamepads disabled` | warn | the backend could not be created; no hub and no probe |
| `tox2: pad: steam overlay loaded\|not loaded[; <NAME>=<value> ...]` | info | right after the backend is created; lists the Steam variables that are set (`SteamGameId`, `SteamOverlayGameId`, `SteamAppId`, `EnableConfiguratorSupport`, `SteamVirtualGamepadInfo`, `SDL_GAMECONTROLLER_IGNORE_DEVICES`, `SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD`); `SteamVirtualGamepadInfo` prints as `set` |
| `tox2: pad: device + <vid:pid> <kinds> rumble yes\|no "<name>"` | info | (GameInput backend only) a device arrived; `<kinds>` is `gamepad`, `controller` or `gamepad+controller` |
| `tox2: pad: device <id> path <pnp path>` | debug | (GameInput backend only) the device's path, with the arrival |
| `tox2: pad: device - <vid:pid>` | info | (GameInput backend only) a device was removed |
| `tox2: pad: haptics: <step> failed: <reason>` | warn | macOS: a Core Haptics step failed for a motor; once per motor |
| `tox2: pad: gamepad <vid:pid> <use>[ (steam slot <n>)]` | info | a device's use changed; `<use>` is `used`, `handled by steam`, `other process` or `not a gamepad` |
| `tox2: pad: gamepad input paused (background)` and `tox2: pad: gamepad input resumed` | debug | the process lost or regained the foreground window |
| `tox2: pad: probe ...` | info | only with `--pad-probe` (below) |

`cellPad` itself logs `tox2: pad: port 0 connected` and `disconnected`. With `--pad gamepad` they
follow the pad being plugged in and out as the device list reports it, but in the game a pad connected after the start
has not worked; otherwise port 0 is always connected, because the keyboard is
part of the pad.

## `--pad-probe`

`--pad-probe` is a diagnostic for bug reports and for checking what Windows and Steam present. It
needs the window, is independent of `--pad`, and does not feed the pad: with `--pad none`, `neutral`
or `keyboard` it only logs. Next to the start lines it logs:

| Line | Meaning |
| --- | --- |
| `tox2: pad: probe <id> <vid:pid> buttons <names> lt <f> rt <f> ls (<x>,<y>) rs (<x>,<y>)` | A reading of any listed device, used by the game or not, whenever the text changes. `<names>` joins the pressed buttons with `+` or is `none`; sticks and triggers are rounded to steps of 0.05 so noise does not produce lines |
| `tox2: pad: probe <id> <vid:pid> no gamepad reading` | Once per device that has no gamepad reading, such as a generic controller |
| `tox2: pad: probe SteamVirtualGamepadInfo unset` (or `unreadable`, `empty`) | Whether the variable of that name is set, and if it names a readable file, one `tox2: pad: probe SteamVirtualGamepadInfo: <line>` per line of its first 4 KiB |

Probe readings follow GameInput's foreground-only policy, so they can be neutral or stale while the
window is in the background. Add `--log-level debug --log FILE` to keep the device paths in a file.

## `tox2_padtool`

`tox2_padtool` exists on Windows only. `build/release/bin/tox2_padtool.exe` uses the same backend without the game. It asks GameInput for
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
