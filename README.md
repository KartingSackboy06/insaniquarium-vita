# Insaniquarium Vita
A PlayStation Vita port of Insaniquarium! Deluxe.  
Not affiliated with PopCap, EA or Sony.

Loads the Android `libmain.so` (armeabi-v7a) of Insaniquarium directly on a PS Vita: maps it into memory,
relocates it against Vita-side replacements for libc/SDL/GL, runs its C++ constructors and calls `SDL_main`.
Plays the PopCap (and optional SOE) intro videos on launch. The first line of `log.txt` shows the loader version.

Credit to [kyle-sylvestre](https://github.com/kyle-sylvestre/WinFish) for the Android version of the game, which is a fork off of Vindirect's original [WinFin](https://github.com/Vindirect/WinFish) decompilation. This build acts as a loader for the libmain.so library from the Android v0.1 release, adding custom button mappings. Future bug fix development for the PlayStation Vita version of the game will be separate to keep code/asset files as efficient as possible. Credit to [rinnegatamante](https://github.com/rinnegatamante/vitagl) for vitaGL.

This repository contains **no game files**. You need your own copy of the game.

## Installation Prerequisites
- A Vita with HENkaku and [kubridge](https://github.com/bythos14/kubridge) installed, and `libshacccg.suprx`
  (needed by vitaGL's runtime shader compiler).
- The game's data files in `ux0:data/insaniquarium/` on the Vita (`properties/`, `images/`, ...).
- You must own your own copy of Insaniquarium! Deluxe.

## Installation Instructions
- Download InsaniquariumVita.vpk from the releases page.
- Install the VPK using VitaShell.
- Find the Insanquarium! Deluxe folder on your PC/Mac.
  - On Mac the path is: '/Users/***/Library/Application Support/Steam/steamapps/common/Insaniquarium Deluxe'
  - On PC the path is: 'C:\Program Files (x86)\Steam\steamapps\common\Insaniquarium Deluxe'
- Copy the folder to your PS Vita using a FTP or USB transfer into 'ux0:data/'
- Rename the folder to 'insaniquarium'
  - Note: not all the files are required, but the total size is 13.7MB so it's convenient to copy it all.
- Have fun! The game will play at 36 fps without overclocking.
  
  

## Controls & Features
| Vita | Game |
|---|---|
| Touchscreen | Feed your fish, collect coins, fight aliens, tap basically anything! Acts as a left click through the whole game. |
| Rear Touchscreen | Useful for feeding fish and fighting aliens. (Harder to collect coins)  |
| X BUTTON | Tap on the glass... |
| SQUARE | Bubbles!!! Linked to "B" |
| TRIANGLE | Opens the Presto Change-O menu. (Functions the same as as right-clicking Presto on PC) |
| CIRCLE | A basic navigation/back button. |
| L/R BUTTONS | Cycle through menu/help/story pages. |
| START | Pauses the game. Linked to "space" |
| D-PAD UP | Hold down to open the keyboard. (Hold for at least 1500ms) |
| CHEATS | Enter with ALL LOWERCASE letters using the keyboard. |
  
WARNING: Do not attempt to open the keyboard while the game is paused. This causes a softlock.

### Intro Videos
- The install features optional intro videos, bundled into the VPK. The videos (H.264 + AAC, 960x544) are located in the `ux0:/app/INSNQ4R13/USRDIR/movies/` folder. A missing video is skipped, so these can be deleted in VitaShell if desired.
- Note: It is possible that `popcap_logo.mp4` is required, but `soe_logo.mp4` can be safely deleted. I haven't tested running the VPK without the videos yet because I like the splash, but theoretically they should be optional.

### Known Glitches:
- Opening the keyboard (via D-PAD UP) while the game is paused causes a softlock.
- `FIXED` The Guppy death sound and Breeder death animation didn't load.

# <ins>Fun Extras for Advanced Users</ins>

### Optional Settings
Create a file at `ux0:data/insaniquarium/loader.cfg` to change values.
One `key=value` per line. Every value that is applied is echoed in `log.txt`.
Default values for each key values are shown on the right.

    game_priority=160          worker_priority=160        cpu_mhz=444
    key_start=32               key_square=98              (ASCII codes, 0 disables)
    cross_right_click=1        (0 disables Cross = right-click)
    triangle_presto=1          (0 disables Triangle = Presto menu)
    circle_close=1             (0 disables Circle = close menus / unpause)
    help_lr=1                  (0 disables L/R on the Help, Story and Hall of Fame screens)
    quit_confirm=1             (0 = the main-menu Quit button exits without asking)
    cheat_keyboard=1           cheat_hold_ms=1500
    zero_memory=1              (0 turns off zero-filled allocations)
    skip_intro=0               (1 skips the videos)
    intro_volume=100           intro_audio_buffer=117     intro_bgm=0

### Source Layout
    src/main.c          startup, fake JNIEnv, game thread, watchdog
    src/so_util.c       ELF loader + relocator (kubridge for executable memory)
    src/wrappers.c      bionic -> newlib shims, SDL event/pad handling, quit button
    src/gl_wrappers.c   float-argument GL wrappers handed out via SDL_GL_GetProcAddress
    src/intro_video.c   intro videos (SceAvPlayer video + audio)
    src/yuv_convert.h   YUV -> RGB converters for the intro video (NEON + scalar)
    src/unwind_hook.c   lets the C++ unwinder find libmain.so's exception tables
    src/import_table.c  generated from imports.txt by tools/gen_import_table.py

### Troubleshooting
`ux0:data/insaniquarium/log.txt` is rewritten on every run. A crash is logged with addresses relative to the 
library (`lib+...`) so it can be looked up in `libmain.so`. Failed file opens are logged; build with `LOADER_DEBUG=ON`
for the full trace, per-second video status and a memory/fps heartbeat.

`ux0:data/insaniquarium/log.txt` is rewritten on every run. A crash is logged with addresses relative to the library
(`lib+...`) so it can be looked up in `libmain.so`. Failed file opens are logged; build with `LOADER_DEBUG=ON` for
the full trace, per-second video status and a memory/fps heartbeat.

### Build Information
Requires Docker. From the project folder:

    ./build.sh                    # release build -> build/InsaniquariumVita.vpk
    LOADER_DEBUG=ON ./build.sh    # same, with verbose diagnostics in log.txt

`build.sh` rebuilds vitaGL from source (pinned commit, `NO_SPLASHSCREEN=1`) so the vitaGL logo does not appear.
The version number lives in one place, `LOADER_VERSION` in `CMakeLists.txt`.
