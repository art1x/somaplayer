# SomaFM Radio

A [SomaFM](https://somafm.com) radio player for the **TrimUI Hammer** running [NextUI](https://github.com/LoveRetro/NextUI), built with the [Apostrophe](https://github.com/Helaas/apostrophe) UI toolkit.

Browse and play SomaFM internet radio stations, save favorites, and keep music playing in the background while you game.

## Features

- **Browse** all SomaFM stations with genre and listener count
- **Favorites** — star stations for quick access
- **Now Playing** — cover art, track and artist metadata
- **Background playback** — music keeps playing after you exit the app
- **Screen timeout** — configurable backlight off timer
- **WiFi management** — enables WiFi on demand if not already connected

## Installation on TrimUI Hammer

Download `SomaFM Radio.zip` from the [latest release](https://github.com/art1x/somaplayer/releases/latest), extract it and copy `SomaFM Radio.pak` into the Tools folder on your SD card.

## Controls

| Button | Action |
|--------|--------|
| ↑ / ↓ | Navigate |
| A | Play / Open Now Playing |
| X | Toggle / Remove favorite |
| Y | Stop playback |
| SELECT | Turn off screen |
| MENU | Settings |
| B | Back / Exit (music keeps playing) |

**Now Playing:** SELECT + A to wake screen after timeout.

## Credits

Built with [Apostrophe](https://github.com/Helaas/apostrophe) — a C UI toolkit for NextUI paks by Kevin Vranken (MIT License).

Audio streamed via [mpg123](https://www.mpg123.de/) (LGPL 2.1). Network via [libcurl](https://curl.se/libcurl/) and [OpenSSL](https://openssl.org/). JSON parsing via [cJSON](https://github.com/DaveGamble/cJSON) (MIT).

Radio streams and metadata provided by [SomaFM](https://somafm.com). Please support them at **[somafm.com/support/](https://somafm.com/support/)** — they are listener-supported and ad-free.

This project was developed primarily with the help of [Claude Code](https://claude.ai/code) by Anthropic.

## License

MIT — see [LICENSE](LICENSE).
