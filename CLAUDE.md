# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
make tg5040        # cross-compile for TrimUI Hammer → build/tg5040/SomaFM Radio.pak/
make linux         # native desktop build for quick iteration (requires SDL2, SDL2_ttf, SDL2_image, libcurl)
make clean         # remove build/
```

Docker (`ghcr.io/loveretro/tg5040-toolchain:latest`) is required for `tg5040`. On Flatpak-based systems the Makefile falls back to `flatpak-spawn --host docker` automatically.

First build downloads and cross-compiles curl/openssl (via `apostrophe/scripts/build_third_party.sh`) and mpg123 (via `scripts/build_mpg123.sh`) — this takes several minutes. Subsequent builds are fast because stamp files cache the results.

`apostrophe/` is a symlink to `../wikireader/apostrophe` and must exist before building.

## Architecture

### Screen flow
```
main()
  └─ screen_main_menu()          [custom render loop — MENU opens Settings]
       ├─ screen_stations()      [ap_list widget]
       │    └─ play_channel() → load_cover() → screen_now_playing()  [custom loop]
       ├─ screen_favorites()     [ap_list widget]
       │    └─ play_channel() → load_cover() → screen_now_playing()
       └─ screen_settings()      [ap_options_list widget]
```

**Key constraint:** `ap_list` and `ap_options_list` intercept `AP_BTN_MENU` internally (shows footer overflow) — it never reaches tertiary_action_button. Any screen that needs to open Settings on MENU must be a custom render loop using `ap_poll_input()`.

### Audio playback (`player.c`)
`mpg123` runs as a child process via `fork()`+`exec()`. The child calls `setpgid(0,0)` immediately after fork so it's in its own process group and survives app exit. `player_detach()` clears the PID without sending SIGTERM, leaving mpg123 running as an orphan. `player_adopt(pid)` re-attaches to a running orphan on next launch.

### Background playback state
When the user exits via **B** in the main menu while music is playing, the app detaches mpg123 and writes `state.txt`. On next launch, `state_restore()` checks if the PID is still alive and re-adopts it. All state files live in `$SHARED_USERDATA_PATH/somaplayer/` (default: `/mnt/SDCARD/.userdata/shared/somaplayer/`):
- `favorites.txt` — station IDs, one per line
- `settings.txt` — `screen_timeout=N`
- `state.txt` — `pid=N` + `channel_id=X` for background playback

### Cover art
Downloaded synchronously via libcurl to `/tmp/soma_cover.png` when a station starts playing. Cached as `g_cover_tex` (SDL_Texture). Uses the `image` field (~120 px) from the SomaFM API, not `largeimage`.

### Screen sleep / timeout
Both `screen_now_playing` and `screen_main_menu` support screen-off via **SELECT** (immediate sleep). In `screen_now_playing` the timer also fires after the configured timeout. Implemented with `SCREEN_ON`/`SCREEN_OFF`/`SCREEN_HINT` states in `screen_now_playing` and a simple `scr_off` flag in `screen_main_menu`. `backlight_off()`/`backlight_on()` use `/dev/disp` ioctl + `/SharedSettings` POSIX SHM (identical to nexttimer). Wake in Now Playing: **SELECT + A**; in Main Menu: any button.

`ap_list`/`ap_options_list` screens (Stations, Favorites, Settings) are blocking widgets — SELECT cannot be intercepted there without modifying the toolkit.

### apostrophe UI toolkit
Header-only (`apostrophe.h` + `apostrophe_widgets.h`). Exactly one `.c` file must define `AP_IMPLEMENTATION` and `AP_WIDGETS_IMPLEMENTATION` before including. Blocking widgets: `ap_list`, `ap_options_list`, `ap_detail_screen`, `ap_confirmation`. Custom loops use `ap_poll_input()` + `ap_draw_*()` + `ap_present()`.

### HTTP vs HTTPS streams
mpg123 on the device may lack TLS support. `soma.c` prefers `http://` entries in the streams array; HTTPS entries are used as fallback. The constructed fallback URL also uses `http://`. The CURL build for the channel-list fetch is full HTTPS with static OpenSSL.

### mpg123 binary
`scripts/build_mpg123.sh` cross-compiles mpg123 1.32.9 for ARM64 with ALSA output. The binary is bundled in the pak (`SomaFM Radio.pak/mpg123`). `player_play()` tries `./mpg123` first, falls back to a system `mpg123`.
