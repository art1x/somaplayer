# SomaFM Radio

A [SomaFM](https://somafm.com) radio player for the **TrimUI Brick/Hammer** running [NextUI](https://github.com/LoveRetro/NextUI), built with the [Apostrophe](https://github.com/Helaas/apostrophe) UI toolkit.

Browse and play SomaFM internet radio stations directly on your handheld.

## Features

- **Single-screen UI** — scrollable station list with full-bleed cover art background
- **Now Playing** — artist and track metadata in the status bar, updated every 30 s
- **Favorites** — star any station with X; favorites shown inline in the list
- **Infinite scroll** — cursor stays centred, list wraps around
- **Left / Right** — switch to the previous or next station and play immediately
- **Sleep timer** — auto-stop playback after 15 / 30 / 60 / 90 minutes
- **Screen timeout** — backlight off after inactivity; reads timeout from NextUI settings
- **Auto-suspend** — device enters S2RAM when screen is off and nothing is playing; wakes and resumes stream automatically
- **WiFi management** — enables WiFi on launch if not already connected

## Installation

Download `SomaFM Radio.zip` from the [latest release](https://github.com/art1x/somaplayer/releases/latest), extract it and copy `SomaFM Radio.pak` into the `Tools/tg5040/` folder on your SD card.

## Controls

| Button | Action |
|--------|--------|
| ↑ / ↓ | Navigate station list |
| ← / → | Previous / Next station (plays immediately) |
| A | Play selected station |
| START | Play / Stop (toggle last played station) |
| X | Toggle favourite ★ |
| SELECT | Sleep timer |
| MENU (hold) | Key hints overlay |
| Power (short) | Toggle screen on / off (audio continues) |
| B | Quit (confirms before exit) |

## Credits

Built with [Apostrophe](https://github.com/Helaas/apostrophe) — a C UI toolkit for NextUI paks by Kevin Vranken (MIT License).

Audio streamed via [mpg123](https://www.mpg123.de/) (LGPL 2.1). Network via [libcurl](https://curl.se/libcurl/) and [OpenSSL](https://openssl.org/). JSON parsing via [cJSON](https://github.com/DaveGamble/cJSON) (MIT).

Radio streams and metadata provided by [SomaFM](https://somafm.com). Please support them at **[somafm.com/support/](https://somafm.com/support/)** — they are listener-supported and ad-free.

This project was developed primarily with the help of [Claude Code](https://claude.ai/code) by Anthropic.

## License

MIT — see [LICENSE](LICENSE).
