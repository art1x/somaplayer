// ================================================================
// main.c  –  SomaFM Radio Player for TrimUI / NextUI
//
// Single-screen UI:
//   Left:       scrollable station list
//   Right:      cover art, full content height, right-aligned,
//               fades to black on the left side
//   Status bar: now-playing info (artist – title) centred with a
//               semi-transparent grey background pill
//
// Controls:
//   ↑ / ↓      → Navigate station list
//   A / START  → Play selected station / Stop playback
//   X          → Toggle favorite ★ for highlighted station
//   MENU (hold)→ Show key-hint overlay with SomaFM support info
//   B          → Ask before exit
//   Power      → Toggle screen on/off (audio continues)
// ================================================================

#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_WIDGETS_IMPLEMENTATION
#include "apostrophe_widgets.h"

#include "soma.h"
#include "player.h"
#include "wifi.h"
#include "favorites.h"
#include "screen.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

// ----------------------------------------------------------------
// Global state
// ----------------------------------------------------------------
static SomaChannelList g_channels    = {0};
static int             g_playing_idx = -1;   /* index of playing channel, -1 = stopped */
static int             g_cursor      = 0;    /* highlighted station in the list */
static FavoriteList    g_favorites   = {0};

static ap_status_bar_opts g_status_bar = {
    .show_clock   = AP_CLOCK_AUTO,
    .show_battery = true,
    .show_wifi    = true,
};

/* Cover art – synchronously loaded when a station starts playing */
static SDL_Texture *g_cover_tex      = NULL;
static int          g_cover_chan_idx = -2;   /* -2 = nothing loaded yet */

/* Now-playing metadata written by a background pthread, read on every frame */
static volatile int    g_np_running  = 0;
static pthread_mutex_t g_np_mutex    = PTHREAD_MUTEX_INITIALIZER;
static char            g_np_title[SOMA_NP_LEN]  = "";
static char            g_np_artist[SOMA_NP_LEN] = "";
static int             g_np_for_idx = -1;

/* UTF-8 strings used as list decorators */
#define MARK_PLAY "\xe2\x96\xb6 "   /* ▶  (U+25B6) */
#define MARK_STAR "\xe2\x98\x85 "   /* ★  (U+2605) */

// ----------------------------------------------------------------
// Userdata directory
// ----------------------------------------------------------------

/* Returns the path to $SHARED_USERDATA_PATH/somaplayer/.
   The result is cached after the first call. */
static const char *userdata_dir(void) {
    static char dir[256];
    if (dir[0]) return dir;
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (!base || !base[0]) base = "/mnt/SDCARD/.userdata/shared";
    snprintf(dir, sizeof(dir), "%s/somaplayer", base);
    return dir;   /* favorites.c already creates this directory */
}

// ----------------------------------------------------------------
// Now-playing background thread
// ----------------------------------------------------------------

/* Polls the SomaFM songs API every 30 seconds and writes the result
   into g_np_title / g_np_artist, protected by g_np_mutex. */
static void *np_poll_thread(void *arg) {
    (void)arg;
    int first = 1;
    while (g_np_running) {
        if (!first) {
            /* Wait 30 seconds between polls, checking the running flag each second
               so the thread stops promptly when playback ends. */
            for (int i = 0; i < 30 && g_np_running; i++)
                sleep(1);
        }
        first = 0;
        if (!g_np_running) break;

        /* Read which channel we should poll (may change if the user switches) */
        pthread_mutex_lock(&g_np_mutex);
        int idx = g_np_for_idx;
        pthread_mutex_unlock(&g_np_mutex);

        if (idx < 0 || idx >= g_channels.count) continue;

        char title[SOMA_NP_LEN]  = "";
        char artist[SOMA_NP_LEN] = "";
        soma_fetch_now_playing(g_channels.channels[idx].id, title, artist);

        /* Only store the result if the channel hasn't changed while we were fetching */
        pthread_mutex_lock(&g_np_mutex);
        if (g_np_for_idx == idx) {
            memcpy(g_np_title,  title,  SOMA_NP_LEN);
            memcpy(g_np_artist, artist, SOMA_NP_LEN);
        }
        pthread_mutex_unlock(&g_np_mutex);
    }
    return NULL;
}

/* Start polling now-playing metadata for channel chan_idx.
   If the thread is already running it keeps running; only the target channel changes. */
static void np_start(int chan_idx) {
    pthread_mutex_lock(&g_np_mutex);
    g_np_for_idx   = chan_idx;
    g_np_title[0]  = '\0';
    g_np_artist[0] = '\0';
    pthread_mutex_unlock(&g_np_mutex);

    if (!g_np_running) {
        g_np_running = 1;
        pthread_t t;
        pthread_create(&t, NULL, np_poll_thread, NULL);
        pthread_detach(t);   /* we never join this thread; it exits when g_np_running = 0 */
    }
}

/* Signal the polling thread to stop and clear cached metadata. */
static void np_stop(void) {
    g_np_running = 0;
    pthread_mutex_lock(&g_np_mutex);
    g_np_for_idx   = -1;
    g_np_title[0]  = '\0';
    g_np_artist[0] = '\0';
    pthread_mutex_unlock(&g_np_mutex);
}

// ----------------------------------------------------------------
// Cover art download + cache
// ----------------------------------------------------------------

/* libcurl write callback: writes downloaded bytes straight to a FILE* */
static size_t cover_write_cb(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    return fwrite(ptr, size, nmemb, fp);
}

/* Download the cover art for channel chan_idx to /tmp/soma_cover.png
   and load it as an SDL texture.  Does nothing if that channel's cover
   is already loaded (g_cover_chan_idx tracks the cache). */
static void load_cover(int chan_idx) {
    if (chan_idx == g_cover_chan_idx) return;   /* already cached */
    if (g_cover_tex) { SDL_DestroyTexture(g_cover_tex); g_cover_tex = NULL; }
    g_cover_chan_idx = chan_idx;

    const char *url = g_channels.channels[chan_idx].image_url;
    if (!url[0]) return;

    const char *tmp = "/tmp/soma_cover.png";
    CURL *curl = curl_easy_init();
    if (!curl) return;
    FILE *fp = fopen(tmp, "wb");
    if (!fp) { curl_easy_cleanup(curl); return; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cover_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef PLATFORM_TG5040
    /* On-device TLS bundle bundled next to the binary */
    curl_easy_setopt(curl, CURLOPT_CAINFO, "./cacert.pem");
#endif
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res == CURLE_OK)
        g_cover_tex = ap_load_image(tmp);
}

// ----------------------------------------------------------------
// Simple info / error dialog
// ----------------------------------------------------------------
static void show_message(const char *title, const char *body) {
    ap_footer_item footer[] = {
        { .button = AP_BTN_A, .label = "OK", .is_confirm = true },
    };
    ap_message_opts opts = {
        .message      = body,
        .footer       = footer,
        .footer_count = 1,
    };
    ap_confirm_result r;
    ap_confirmation(&opts, &r);
    (void)title;
}

// ----------------------------------------------------------------
// Last-played station persistence
// ----------------------------------------------------------------

/* Save the channel ID of chan_idx to last_station.txt so the next
   launch can restore the cursor to the same station. */
static void save_last_station(int chan_idx) {
    if (chan_idx < 0 || chan_idx >= g_channels.count) return;
    char path[280];
    snprintf(path, sizeof(path), "%s/last_station.txt", userdata_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", g_channels.channels[chan_idx].id);
    fclose(f);
}

/* Read last_station.txt and find the corresponding index in g_channels.
   Returns -1 if the file doesn't exist or the ID is not in the list. */
static int load_last_station(void) {
    char path[280];
    snprintf(path, sizeof(path), "%s/last_station.txt", userdata_dir());
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char id[64] = "";
    fscanf(f, " %63s", id);
    fclose(f);
    if (!id[0]) return -1;
    for (int i = 0; i < g_channels.count; i++)
        if (strcmp(g_channels.channels[i].id, id) == 0)
            return i;
    return -1;   /* station no longer in the API response */
}

// ----------------------------------------------------------------
// Playback helper
// ----------------------------------------------------------------

/* Stop whatever is playing, start channel idx, wait briefly to detect
   immediate errors.  Returns 1 on success, 0 on error (already shown). */
static int play_channel(int idx) {
    player_stop();
    np_stop();
    if (!player_play(g_channels.channels[idx].stream_url)) {
        show_message("Error", "Could not start playback.\nIs mpg123 available?");
        return 0;
    }
    usleep(300 * 1000);
    if (player_status() == PLAYER_ERROR) {
        show_message("Error",
            "Playback failed.\nCheck mpg123 is installed\nand the stream is reachable.");
        g_playing_idx = -1;
        return 0;
    }
    g_playing_idx = idx;
    np_start(idx);
    save_last_station(idx);   /* persist so next launch resumes here */
    return 1;
}

// ----------------------------------------------------------------
// Render helpers
// ----------------------------------------------------------------

/* Solid black frame – shown while the screen timeout has blanked the display */
static void render_screen_off(void) {
    ap_color blk = {0, 0, 0, 255};
    ap_draw_rect(0, 0, ap_get_screen_width(), ap_get_screen_height(), blk);
    ap_present();
}

/* Full-screen overlay drawn on top of render_main() while MENU is held.
   Shows key hints and the SomaFM support URL.
   Does NOT call ap_present() – the caller handles that. */
static void render_key_hint_overlay(void) {
    int sw = ap_get_screen_width();
    int sh = ap_get_screen_height();
    TTF_Font *flg = ap_get_font(AP_FONT_LARGE);
    TTF_Font *fsm = ap_get_font(AP_FONT_SMALL);

    /* Semi-transparent dark veil over the whole screen */
    ap_color bg = {0, 0, 0, 210};
    ap_draw_rect(0, 0, sw, sh, bg);

    /* Key-hint lines */
    const char *hints[] = {
        "A / START   \xe2\x80\x94   Play / Stop",
        "\xe2\x86\x91 / \xe2\x86\x93     \xe2\x80\x94   Station navigieren",
        "X           \xe2\x80\x94   Favorit \xe2\x98\x85 umschalten",
        "B           \xe2\x80\x94   Beenden",
    };
    int hint_count = 4;

    /* Measure the total block height so we can centre it vertically */
    int title_h = flg ? TTF_FontHeight(flg) + AP_S(20) : AP_S(36);
    int line_h  = fsm ? TTF_FontHeight(fsm) + AP_S(10) : AP_S(28);
    int url_h   = fsm ? TTF_FontHeight(fsm) + AP_S(14) : AP_S(22);
    int total_h = title_h + hint_count * line_h + url_h;

    int y     = (sh - total_h) / 2;
    int pad_x = AP_S(80);

    /* Heading: "Support SomaFM ♥" */
    if (flg) {
        ap_color yellow = {255, 220, 60, 255};
        ap_draw_text(flg, "Support SomaFM \xe2\x99\xa5", pad_x, y, yellow);
        y += title_h;
    }

    /* Key hints */
    ap_color hint_col = {210, 210, 210, 255};
    for (int i = 0; i < hint_count; i++) {
        if (fsm) ap_draw_text(fsm, hints[i], pad_x, y, hint_col);
        y += line_h;
    }

    /* Donation URL */
    if (fsm) {
        ap_color grey = {140, 140, 140, 255};
        y += AP_S(4);
        ap_draw_text(fsm, "somafm.com/support/", pad_x, y, grey);
    }
}

/* Draw the single main screen:
   – black background
   – cover art right-aligned at full content height, fading to black on the left
   – status bar (with now-playing pill centred inside it)
   – scrollable station list on the left
   – footer buttons
   Does NOT call ap_present() so the caller can layer overlays on top. */
static void render_main(void) {
    int sw   = ap_get_screen_width();
    int sh   = ap_get_screen_height();
    int sb_h = ap_get_status_bar_height();
    int ft_h = AP_S(44);                /* footer height */
    int cont_top = sb_h;
    int cont_bot = sh  - ft_h;
    int cont_h   = cont_bot - cont_top;

    ap_theme *thm  = ap_get_theme();
    TTF_Font *fxsm = ap_get_font(AP_FONT_TINY);

    /* ── Black background ──────────────────────────────────────── */
    ap_color black = {0, 0, 0, 255};
    ap_draw_rect(0, 0, sw, sh, black);

    /* ── Cover art: right-aligned, square, fills content height ── */
    /* SomaFM artwork is square.  We set the drawn size to cont_h × cont_h
       so the image fills the full height of the content area. */
    int cover_sz = cont_h;
    int cover_x  = sw - cover_sz;   /* flush against the right edge */

    if (g_cover_tex) {
        ap_draw_image(g_cover_tex, cover_x, cont_top, cover_sz, cont_h);
    }

    /* ── Gradient: black on left, transparent on right ─────────── */
    /* Draw 48 vertical strips over the cover area.  The leftmost strip is
       fully opaque black; opacity decreases to 0 at the right edge.
       This makes the cover bleed into the black background smoothly. */
    {
        int steps  = 48;
        int grad_w = cover_sz;
        for (int i = 0; i < steps; i++) {
            int   x = cover_x + (grad_w * i       / steps);
            int   w = cover_x + (grad_w * (i + 1) / steps) - x;
            if (w < 1) w = 1;
            Uint8 a = (Uint8)(255 - (255 * i / (steps - 1)));
            ap_color c = {0, 0, 0, a};
            ap_draw_rect(x, cont_top, w, cont_h, c);
        }
    }

    /* ── Status bar (battery / clock / wifi) ───────────────────── */
    ap_draw_status_bar(&g_status_bar);

    /* ── Now-playing pill in the centre of the status bar ───────── */
    /* Read metadata written by the background polling thread. */
    {
        char np_title[SOMA_NP_LEN]  = "";
        char np_artist[SOMA_NP_LEN] = "";
        pthread_mutex_lock(&g_np_mutex);
        memcpy(np_title,  g_np_title,  SOMA_NP_LEN);
        memcpy(np_artist, g_np_artist, SOMA_NP_LEN);
        pthread_mutex_unlock(&g_np_mutex);

        if (fxsm && (np_title[0] || np_artist[0])) {
            /* Build "♪ Artist – Title" (or just one if only one exists) */
            char np_buf[SOMA_NP_LEN * 2 + 8];
            if (np_artist[0] && np_title[0])
                snprintf(np_buf, sizeof(np_buf),
                         "\xe2\x99\xaa %s \xe2\x80\x93 %s", np_artist, np_title);
            else if (np_artist[0])
                snprintf(np_buf, sizeof(np_buf), "\xe2\x99\xaa %s", np_artist);
            else
                snprintf(np_buf, sizeof(np_buf), "\xe2\x99\xaa %s", np_title);

            int text_w = ap_measure_text(fxsm, np_buf);
            int text_h = TTF_FontHeight(fxsm);

            /* Centre the pill horizontally in the screen, vertically in the status bar */
            int tx = (sw - text_w) / 2;
            int ty = (sb_h - text_h) / 2;

            int pad = AP_S(6);
            ap_color np_bg = {40, 40, 40, 180};
            ap_draw_rect(tx - pad, ty - pad / 2, text_w + 2 * pad, text_h + pad, np_bg);

            ap_color np_fg = {220, 220, 220, 255};
            ap_draw_text(fxsm, np_buf, tx, ty, np_fg);
        }
    }

    /* ── Stations list (left column) ────────────────────────────── */
    if (fxsm && g_channels.count > 0) {
        int list_pad_x = AP_S(12);
        /* Keep list text away from the cover; use the full left portion */
        int list_maxw  = cover_x - AP_S(8);
        int item_h     = TTF_FontHeight(fxsm) + AP_S(5);
        int visible    = cont_h / item_h;   /* how many items fit on screen */

        /* Scroll offset: keep g_cursor within the visible window */
        int scroll = 0;
        if (g_cursor >= visible) scroll = g_cursor - visible + 1;

        for (int i = 0; i < visible; i++) {
            int ch_i = scroll + i;
            if (ch_i >= g_channels.count) break;

            SomaChannel *ch      = &g_channels.channels[ch_i];
            bool         sel     = (ch_i == g_cursor);
            bool         playing = (ch_i == g_playing_idx);
            bool         fav     = favorites_contains(&g_favorites, ch->id);

            int iy = cont_top + i * item_h;

            /* Highlight bar behind the selected item */
            if (sel) {
                ap_color hl = thm->highlight;
                hl.a = 160;   /* semi-transparent so gradient shows through */
                ap_draw_rect(0, iy, cover_x, item_h, hl);
            }

            /* "▶ ★ Station Name" */
            char label[128] = "";
            if (playing) strcat(label, MARK_PLAY);
            if (fav)     strcat(label, MARK_STAR);
            strncat(label, ch->title, sizeof(label) - strlen(label) - 1);

            ap_color tc = sel ? thm->highlighted_text : thm->text;
            ap_draw_text_ellipsized(fxsm, label, list_pad_x, iy, tc, list_maxw);
        }
    }

    /* ── Footer ─────────────────────────────────────────────────── */
    {
        ap_footer_item footer_play[] = {
            { .button = AP_BTN_B, .label = "Exit"                      },
            { .button = AP_BTN_X, .label = "\xe2\x98\x85"              },
            { .button = AP_BTN_A, .label = "Play", .is_confirm = true  },
        };
        ap_footer_item footer_stop[] = {
            { .button = AP_BTN_B, .label = "Exit"                      },
            { .button = AP_BTN_X, .label = "\xe2\x98\x85"              },
            { .button = AP_BTN_A, .label = "Stop", .is_confirm = true  },
        };
        if (g_playing_idx >= 0)
            ap_draw_footer(footer_stop, 3);
        else
            ap_draw_footer(footer_play, 3);
    }
}

// ----------------------------------------------------------------
// Main screen loop – the only screen in the app
// ----------------------------------------------------------------
static void screen_main(void) {
    bool     show_hint  = false;
    uint32_t last_ticks = SDL_GetTicks();

    /* Pre-load the cover for the initial cursor position so something
       is visible right away when the user opens the app. */
    if (g_cursor >= 0 && g_cursor < g_channels.count)
        load_cover(g_cursor);

    while (1) {
        /* ── Screen timeout timer ────────────────────────────────── */
        uint32_t now_ticks = SDL_GetTicks();
        screen_update(now_ticks - last_ticks);
        last_ticks = now_ticks;

        /* ── Input ────────────────────────────────────────────────── */
        ap_input_event ev;
        while (ap_poll_input(&ev)) {
            /* MENU: show overlay on press, hide on release.
               Handle this regardless of screen-off state so the overlay
               appears the moment the button is pressed. */
            if (ev.button == AP_BTN_MENU) {
                show_hint = ev.pressed;
                continue;
            }

            if (!ev.pressed) continue;   /* ignore button-up events for all others */

            /* While the screen is off, swallow all input.
               The power button is handled by its own thread in screen.c. */
            if (screen_is_off()) continue;

            screen_activity();   /* reset the idle timer */

            switch (ev.button) {

                case AP_BTN_UP:
                    if (g_cursor > 0) g_cursor--;
                    break;

                case AP_BTN_DOWN:
                    if (g_cursor < g_channels.count - 1) g_cursor++;
                    break;

                case AP_BTN_A:
                case AP_BTN_START:
                    if (g_playing_idx >= 0) {
                        /* Stop the currently playing station */
                        player_stop();
                        np_stop();
                        g_playing_idx = -1;
                    } else {
                        /* Play the highlighted station */
                        if (play_channel(g_cursor))
                            load_cover(g_cursor);
                    }
                    break;

                case AP_BTN_X:
                    /* Toggle favourite for the highlighted station */
                    if (g_cursor >= 0 && g_cursor < g_channels.count)
                        favorites_toggle(&g_favorites, g_channels.channels[g_cursor].id);
                    break;

                case AP_BTN_B: {
                    /* Confirm before quitting */
                    ap_footer_item footer[] = {
                        { .button = AP_BTN_B, .label = "Cancel"                   },
                        { .button = AP_BTN_A, .label = "Exit", .is_confirm = true },
                    };
                    ap_message_opts opts = {
                        .message      = "Exit SomaFM Radio?",
                        .footer       = footer,
                        .footer_count = 2,
                    };
                    ap_confirm_result r;
                    ap_confirmation(&opts, &r);
                    if (r.confirmed) {
                        player_stop();
                        np_stop();
                        return;   /* back to main() → cleanup */
                    }
                    break;
                }

                default: break;
            }
        }

        /* ── Render ────────────────────────────────────────────── */
        if (screen_is_off()) {
            render_screen_off();          /* solid black + ap_present */
            SDL_Delay(50);
        } else {
            render_main();                /* draws everything except overlay */
            if (show_hint)
                render_key_hint_overlay();/* layered on top when MENU is held */
            ap_present();
            SDL_Delay(16);                /* ~60 fps */
        }
    }
}

// ----------------------------------------------------------------
// main
// ----------------------------------------------------------------
int main(void) {
    ap_config cfg = {
        .window_title = "SomaFM Radio",
        .font_path    = AP_PLATFORM_IS_DEVICE ? NULL : "apostrophe/res/font.ttf",
        .is_nextui    = AP_PLATFORM_IS_DEVICE,
        .cpu_speed    = AP_CPU_SPEED_NORMAL,
    };
    if (ap_init(&cfg) != AP_OK) return 1;
    ap_set_power_handler(false);   /* we manage the power button ourselves in screen.c */
    screen_init();
    screen_init_power();

    soma_init();
    player_init();
    favorites_load(&g_favorites);

    /* Enable WiFi if it isn't already active */
    if (!wifi_is_active()) {
        show_message("WiFi", "Enabling WiFi, please wait...");
        if (!wifi_enable()) {
            show_message("WiFi Error",
                "Could not connect to WiFi.\nCheck your WiFi settings.");
            goto cleanup;
        }
    }

    /* Fetch the full station list from the SomaFM API */
    if (!soma_fetch_channels(&g_channels) || g_channels.count == 0) {
        show_message("Network Error",
            "Could not load station list.\nCheck your internet connection.");
        goto cleanup;
    }

    /* Position the cursor at the last played station (or fall back to station 0) */
    {
        int last = load_last_station();
        g_cursor = (last >= 0 && last < g_channels.count) ? last : 0;
    }

    screen_main();

cleanup:
    player_cleanup();
    soma_cleanup();
    screen_quit_power();
    ap_quit();
    return 0;
}
