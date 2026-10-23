# HZDRawMouse — raw mouse camera for Horizon Zero Dawn (PC)

An ASI plugin that makes the third-person camera in **Horizon Zero Dawn Complete Edition** follow
your mouse 1:1, every rendered frame, at any frame rate.

The stock PC camera feels like it is interpolated at a low tick rate and slightly delayed: fast
circles turn into straight segments, and turning lags behind the mouse. That is what the game does:

- the mouse is only polled every **33.3 ms** (30 Hz), and the last value is reused in between;
- the camera is simulated on a fixed **29.97 Hz** tick, with a gamepad-style response curve and an
  angular acceleration limit applied to the mouse;
- each rendered frame shows a blend between the **last two** tick poses, so the view is always about
  one tick (≈33 ms) behind and moves in straight lines between ticks.

HZDRawMouse removes all three for mouse look. The rendered camera turns by exactly your mouse counts
every frame. The game's own camera simulation still runs, kept in sync with what you see, so aiming,
the crosshair and collisions behave as before.

## Requirements

- Horizon Zero Dawn **Complete Edition** for PC, latest patch (Steam, 2022 build). Other store
  versions are untested. The plugin finds the game code by pattern, not by a fixed exe version: if
  something does not match, it disables itself and logs why. It does not crash the game.
- **Not** for *Horizon Zero Dawn Remastered* (a separate game).
- Windows 10/11 x64.

## Install

Two archives are on the releases page:

- **`HorizonZeroDawn-RawMouseInputFix-v1.0.1-with-Ultimate-ASI-Loader-v9.7.4.zip`**: if the game has
  no ASI loader yet. Copy `winmm.dll`, `HZDRawMouse.asi` and `HZDRawMouse.ini` into the game folder,
  next to `HorizonZeroDawn.exe` (Steam: right-click the game → Manage → Browse local files).
  `winmm.dll` is [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) x64 v9.7.4.
- **`HorizonZeroDawn-RawMouseInputFix-v1.0.1-plugin-only.zip`**: if Ultimate ASI Loader is already
  installed as `winmm.dll`. Copy only `HZDRawMouse.asi` and `HZDRawMouse.ini`.

Each archive has a `SHA256SUMS.txt`, and a `.sha256` file is published next to each archive.
Start the game: the raw camera is on by default.

**Why `winmm.dll`?** HZD always loads `version.dll` from `C:\Windows\System32`, because of a Windows
compatibility shim Microsoft applies to this game. So an ASI loader named `version.dll` never runs.
`winmm.dll` works. If you already use Ultimate ASI Loader for another mod, rename it to `winmm.dll`;
there is no need for a second copy.

## Use

- **F9** toggles between the raw camera (high beep) and the game's original camera (low beep).
- Settings are in `HZDRawMouse.ini`: start mode, toggle key, beep, log.

**Sensitivity:** the raw camera uses a single linear scale, the one the game already uses while
aiming. Hip-fire turning speed may therefore feel a little different from the original camera.
Adjust it with the in-game mouse sensitivity sliders.

## Uninstall

Delete `HZDRawMouse.asi` and `HZDRawMouse.ini`. Also delete `winmm.dll` if no other ASI mod uses it.
Nothing else is changed: the game files and saves are never modified.

## Troubleshooting

- `HZDRawMouse.log` (next to the plugin) lists the version and every hook found at startup.
- **No log file at all:** the loader did not run. Check that `winmm.dll` is in the game folder and is
  the x64 Ultimate ASI Loader.
- **A line saying a site was "not found" / "disabled":** your game build differs. Please open an
  issue and attach the log.

## Known limitations

- Only the camera *view* is raw. The game's own extra camera rotation (follow alignment while
  walking, aim magnetism) and the character, aiming logic and animation still run on the game's
  29.97 Hz tick with its interpolation, as before.
- Cutscenes, photo mode and other scripted cameras are left untouched.
- Some latency remains in the game's frame pipeline. The plugin adds none: it refreshes the camera
  right after the mouse is read, before the frame's camera is interpolated.

## How it works

All targets are found at startup by pattern (MSVC RTTI for vtables, the import table, instruction
shapes), and checked once. Every check that fails disables only its own feature. There is no exe
hash or version lock. In short:

1. `KeyBoardController` poll: the 33.3 ms throttle constant is set to 0 and the raw counts are
   accumulated exactly once per camera tick.
2. Camera look input: the N-tick averaging and slew limiter are bypassed; the camera mode's look
   response curve is linearised and its angular acceleration limit is disabled.
3. Camera entity: the camera tick's own `SetWorldTransform` call is redirected. Both render
   interpolation endpoints (`entity+0xE0` → `entity+0x120`) are written as the latest tick pose
   (and the previous one advanced by the look that tick applied), each rotated by the counts not
   yet applied. They are refreshed every frame, right after the mouse drain, so mouse rotation is
   exact at any interpolation factor. While the game itself ignores mouse look (menus over the
   running world, such as looting), no offset is applied.
4. Safety: the camera entity is only written under its own lock, and a hook on the camera entity
   destructor drops the tracked camera before it is freed (loading screens, quitting).

## Build from source

Visual Studio 2022/2026 with the C++ workload (it includes CMake and Ninja):

```
build.bat                    # build\HZDRawMouse.asi (Release, x64)
build\scan_test.exe "<game>\HorizonZeroDawn.exe" <out dir>   # offline check of every hook site
powershell -File package-release.ps1   # dist\...-plugin-only.zip and ...-with-Ultimate-ASI-Loader-v9.7.4.zip
```

`deploy.bat` copies a development build and `config\HZDRawMouse.dev.ini` (diagnostics on) into the
game folder.

## Credits

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG, MIT
  license. Its notice ships as `THIRD-PARTY-NOTICES.txt` in the archive that includes the loader.

## License

MIT, see [LICENSE](LICENSE).

## Misc

For more game fixes, visit: https://slop-blog.enkhayzomachines.net/fixes :)
