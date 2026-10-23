Horizon Zero Dawn Raw Mouse Input Fix v@VERSION@
=================================================

Supported game build
--------------------
Horizon Zero Dawn Complete Edition for Windows x64, final patch (Steam,
version 1.0.11.14), HorizonZeroDawn.exe SHA-256:
866C131C0BBE6E60DBF4332618BBC2109E60F6620106CFF925D7A5399220AECA

This hash identifies the reference build; it is not a hard allowlist. At
startup the plugin locates every hook by pattern (RTTI vtables, the import
table, instruction shapes) and checks each one once. A check that fails
disables only its own feature and is reported in HZDRawMouse.log; the game is
never blocked. Horizon Zero Dawn Remastered is a different game and is not
supported.

What it fixes
-------------
The stock camera polls the mouse only every 33.3 ms and reuses the value in
between, simulates the camera on a fixed 29.97 Hz tick with a gamepad-style
response curve and an angular acceleration limit, and renders each frame as a
blend between the last two tick poses: always about one tick behind, moving in
straight lines between ticks.

With the fix, the rendered camera turns by exactly your mouse counts every
frame, at any frame rate. The game's camera simulation still runs and is kept
in sync with what you see.

Choose an installation
----------------------
Plugin-only archive:
  Requires Ultimate ASI Loader x64 already installed in the game folder as
  winmm.dll. Copy HZDRawMouse.asi and HZDRawMouse.ini beside
  HorizonZeroDawn.exe, or into the loader's scripts or plugins folder. Keep
  both files together.

Convenience archive with Ultimate ASI Loader:
  If the game has no ASI loader, copy winmm.dll, HZDRawMouse.asi, and
  HZDRawMouse.ini beside HorizonZeroDawn.exe. The bundled winmm.dll is
  Ultimate ASI Loader x64 v9.7.4 (its dinput8.dll, renamed).

  If the game already has Ultimate ASI Loader as winmm.dll, do not overwrite
  it. Copy only HZDRawMouse.asi and HZDRawMouse.ini. The fix itself does not
  need to own the loader.

Why winmm.dll: Windows applies a compatibility shim to HorizonZeroDawn.exe
that loads version.dll from System32 before the game folder is searched, so a
loader named version.dll never runs in this game, and the game never loads
dinput8.dll. A loader named winmm.dll is loaded from the game folder.

For either installation:
1. Close Horizon Zero Dawn before copying files.
2. Launch the game normally.

Usage
-----
The raw camera is ON by default; no key press is needed. Press F9 in game only
for an A/B comparison: it switches to the game's original mouse camera (low
beep), and pressing it again switches back to the raw camera (high beep).

The raw camera turns on one linear scale, the one the game already uses while
aiming, so hip-fire turning may feel a little different from the original
camera. Adjust it with the in-game mouse sensitivity sliders.

HZDRawMouse.ini: StartMode (5 = raw camera, 0 = original), ToggleKey
(virtual-key code, 0x78 = F9, 0 disables it), Beep, and the log.

Safety and scope
----------------
Only the rendered camera view is changed. The game simulation, aiming logic,
character, and animation still run on the native 29.97 Hz tick. The game's own
extra camera rotation (follow alignment while walking, aim magnetism) keeps
the game's interpolation. While the game itself ignores mouse look (looting a
machine, menus over the running world), the view is left to the game.
Cutscenes, photo mode, and other scripted cameras are left untouched. Game
files and saves are never modified.

Diagnostics
-----------
The plugin writes HZDRawMouse.log beside the ASI with its version and every
hook found at startup. Check it if the fix does not activate. If there is no
log at all, the loader did not run: check that winmm.dll is in the game
folder.

Uninstallation
--------------
Close the game and remove HZDRawMouse.asi and HZDRawMouse.ini. The optional
HZDRawMouse.log diagnostic file may also be removed.

Do not remove winmm.dll if it existed before this fix or if another plugin
depends on it. Remove the bundled loader only when you installed that exact
file for this fix and no other ASI plugin needs it.

Third-party component
---------------------
The explicitly named convenience archive includes Ultimate ASI Loader v9.7.4
by ThirteenAG under the MIT License, renamed to winmm.dll. See
THIRD-PARTY-NOTICES.txt. The plugin-only archive contains no third-party
loader binary.

Misc
----
For more game fixes, visit: https://slop-blog.enkhayzomachines.net/fixes :)
