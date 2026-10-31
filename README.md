# uevr-stick-fix

A [UEVR](https://github.com/praydog/UEVR) plugin that keeps each VR thumbstick working while the other controller is asleep.

## The problem

In UEVR's OpenXR mode, a thumbstick can stop working whenever the *other* controller goes idle. Typical case: you're turning with the right stick while your left hand rests, and turning stops until you move the left controller again. Moving the left controller brings it straight back. The aim and pointer from the stick's own controller keep working throughout.

Seen on a Quest 3 over Virtual Desktop, where the UEVR log shows `XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED` each time it starts and stops. Turning off Quest hand tracking doesn't help: the controller is going to sleep, not switching to hands.

## The cause

`OpenXR::get_stick_axis` (`src/mods/vr/runtimes/OpenXR.cpp`) finds the stick's binding under `get_current_interaction_profile()`. That function always asks for the interaction profile of `hands[0]`, the **left** hand, whichever stick is being read. When one controller sleeps, the runtime changes or drops that hand's profile, the lookup fails, and UEVR reads the other stick as centred.

The proper fix belongs in UEVR: look up the profile of the hand being read, e.g. `xrGetCurrentInteractionProfile(session, hands[hand_idx].path, ...)`. This plugin works around it until then.

## What the plugin does

UEVR fills in the game's XInput state from the VR controllers and then passes it to plugins. For each stick UEVR left at zero, the plugin:

1. asks the OpenXR runtime directly for UEVR's own `joystick` action, filtered to that hand (`xrGetActionStateVector2f` with the hand's subaction path), which doesn't involve the interaction profile;
2. applies UEVR's joystick deadzone and scaling, and writes the value in.

When UEVR already has a value, the plugin does nothing. Sticks that UEVR zeroes on purpose are left alone: while UEVR's menu is open, during d-pad shifting (all methods), and for the snap-turn stick when snap turn is on. It reads these settings from the game profile's `config.txt` and picks up changes while the game runs.

UEVR links its OpenXR loader into `UEVRBackend.dll`, so a separate `openxr_loader.dll` in the process rejects UEVR's handles. The plugin therefore gets its OpenXR functions from the runtime DLL itself, through the standard `xrNegotiateLoaderRuntimeInterface` entry point every runtime exports.

## Requirements

- UEVR in OpenXR mode, with plugin interface 2.x (current nightlies). OpenVR mode reads sticks differently and doesn't have this bug; the plugin logs that and does nothing.
- 64-bit Windows.

Tested on Quest 3 with Virtual Desktop (VDXR). Other OpenXR runtimes (Quest Link, SteamVR, WMR) should work, as the method is part of the OpenXR loader standard, but haven't been tested. Reports welcome.

## Install

Copy `stick_fix.dll` into the game's UEVR profile `plugins` folder:

```
%APPDATA%\UnrealVRMod\<GameExeName>\plugins\
```

To remove it, delete the file.

The UEVR log (`log.txt` in the same profile folder) shows:

```
[stick_fix] loaded
[stick_fix] settings: deadzone 0.20, swap off, d-pad shifting on (method 0), snap turn off
[stick_fix] OpenXR runtime: C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.dll
[stick_fix] ready
[stick_fix] UEVR read the right stick as centred; filled in (-0.95, 0.31) from the runtime
```

The last line appears (up to three times per stick) when the plugin covers for the bug.

## Limitations

- Snap turn also reads the stick through the affected code, so with snap turn on, the plugin can't restore snap turning while the other controller sleeps. It leaves that stick alone.
- With the thumbrest d-pad methods, the plugin checks the thumbrest through UEVR's per-hand action check. If that controller is the one asleep, d-pad shifting can't be detected, but then its stick can't be used either.

## Build

With the MSVC Build Tools (x64):

```
build.bat
```

If `vcvars64.bat` isn't at the Build Tools 18 default path, set `VCVARS` to it first. Or use CMake:

```
cmake -B out -A x64
cmake --build out --config Release
```

The DLL uses the static CRT and depends only on `KERNEL32.dll`.

## Licence

MIT; see [LICENSE](LICENSE). Bundled headers keep their own licences; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
