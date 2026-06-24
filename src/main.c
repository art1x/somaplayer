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
#include <sys/stat.h>

// ----------------------------------------------------------------
// Global state
// ----------------------------------------------------------------
static SomaChannelList g_channels         = {0};
static int             g_playing_idx      = -1;   /* index of playing channel, -1 = stopped */
static int             g_last_played_idx  = -1;   /* last station that was playing, for START resume */
static int             g_cursor           = 0;    /* highlighted station in the list */
static FavoriteList    g_favorites        = {0};

/* Path to the on-disk channels JSON cache; set once in main() after
   the userdata directory is known.  Used by cache_refresh_thread(). */
static char g_channels_cache_path[320] = "";

static ap_status_bar_opts g_status_bar = {
    .show_clock   = AP_CLOCK_AUTO,
    .show_battery = true,
    .show_wifi    = true,
};

/* Cover art – downloaded in a background thread; texture created in main thread */
static SDL_Texture  *g_cover_tex      = NULL;
static int           g_cover_chan_idx = -2;   /* channel whose cover is displayed */
static volatile int  g_cover_loading  = 0;    /* download in progress */
static volatile int  g_cover_dl_done  = 0;    /* file saved, needs texture creation */
static          int  g_cover_dl_idx   = -1;   /* channel that was just downloaded */
static          char g_cover_dl_path[320] = ""; /* path of the ready-to-load file */


/* Sleep timer: milliseconds remaining; 0 = off */
static uint32_t g_sleep_timer_ms = 0;

/* Auto-suspend timeout: milliseconds of screen-off + no playback before
   calling the system suspend script.  0 = disabled. */
static uint32_t g_suspend_timeout_ms = 0;

/* Description pill: hide after 10 s of no navigation; reset on every cursor move */
static uint32_t g_desc_hide_at = 0;

/* Now-playing ticker scroll state */
static uint32_t g_ticker_ms   = 0;
static int      g_ticker_px   = 0;
static char     g_ticker_last[SOMA_NP_LEN * 2 + 8] = "";

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

        /* Only overwrite if channel unchanged and new data is non-empty */
        pthread_mutex_lock(&g_np_mutex);
        if (g_np_for_idx == idx && (title[0] || artist[0])) {
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
    g_np_for_idx = chan_idx;
    /* Keep old title/artist visible until the new station's first poll returns */
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
    g_np_for_idx = -1;
    /* Keep title/artist — last known song stays visible after stopping */
    pthread_mutex_unlock(&g_np_mutex);
}

// ----------------------------------------------------------------
// Channel cache background refresh
// ----------------------------------------------------------------

/* Fetches a fresh channel list from the SomaFM API in the background
   and saves it to the on-disk cache so the next launch can skip the
   network wait entirely.  We deliberately do NOT update g_channels at
   runtime — that avoids any thread-safety issues with the now-playing
   thread, which reads g_channels without locking. */
static void *cache_refresh_thread(void *arg) {
    (void)arg;
    soma_channels_cache_refresh(g_channels_cache_path);
    return NULL;
}

/* Launch the background cache refresh (fire-and-forget).
   Called once after a successful cache-hit startup so the next session
   gets an up-to-date station list without the user waiting. */
static void start_cache_refresh(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, cache_refresh_thread, NULL) == 0)
        pthread_detach(t);
}

// ----------------------------------------------------------------
// Cover art download + cache
// ----------------------------------------------------------------

/* libcurl write callback: writes downloaded bytes straight to a FILE* */
static size_t cover_write_cb(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    return fwrite(ptr, size, nmemb, fp);
}

/* Background thread: loads or downloads cover art.
   Checks the local cache first; only fetches from network if absent.
   Sets g_cover_dl_done when the file is ready so the main thread creates the texture.
   SDL textures MUST be created on the main thread. */
typedef struct {
    int  idx;
    char url[SOMA_URL_LEN];
    char cache_path[320];
} CoverDlArg;

static void *cover_dl_thread(void *arg) {
    CoverDlArg *a = arg;
    int  idx = a->idx;
    char cache_path[320];
    memcpy(cache_path, a->cache_path, sizeof(cache_path));
    char url[SOMA_URL_LEN];
    memcpy(url, a->url, SOMA_URL_LEN);
    free(a);

    /* Use cached file if it already exists */
    if (access(cache_path, F_OK) != 0) {
        /* Not cached — download now */
        CURL *curl = curl_easy_init();
        if (!curl) { g_cover_loading = 0; return NULL; }
        FILE *fp = fopen(cache_path, "wb");
        if (!fp) { curl_easy_cleanup(curl); g_cover_loading = 0; return NULL; }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cover_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef PLATFORM_TG5040
        curl_easy_setopt(curl, CURLOPT_CAINFO, "./cacert.pem");
#endif
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        fclose(fp);

        if (res != CURLE_OK) {
            remove(cache_path);   /* delete incomplete file */
            g_cover_loading = 0;
            return NULL;
        }
    }

    /* File is ready (cached or freshly downloaded) */
    memcpy(g_cover_dl_path, cache_path, sizeof(g_cover_dl_path));
    g_cover_dl_idx  = idx;
    g_cover_dl_done = 1;
    g_cover_loading = 0;
    return NULL;
}

/* Start an async cover load for channel chan_idx.
   Serves from local cache when available; downloads otherwise. */
static void load_cover(int chan_idx) {
    if (chan_idx == g_cover_chan_idx && !g_cover_loading) return;
    if (g_cover_loading) return;

    /* Clear stale texture immediately */
    if (g_cover_tex) { SDL_DestroyTexture(g_cover_tex); g_cover_tex = NULL; }
    g_cover_chan_idx = chan_idx;
    g_cover_dl_done  = 0;

    const char *url = g_channels.channels[chan_idx].image_url;
    if (!url[0]) return;

    /* Build cache path: userdata_dir/covers/<id>.png */
    char covers_dir[300];
    snprintf(covers_dir, sizeof(covers_dir), "%s/covers", userdata_dir());
    mkdir(covers_dir, 0755);   /* create directory if absent; ignore error if exists */

    CoverDlArg *arg = malloc(sizeof(CoverDlArg));
    if (!arg) return;
    arg->idx = chan_idx;
    memcpy(arg->url, url, SOMA_URL_LEN);
    snprintf(arg->cache_path, sizeof(arg->cache_path), "%s/%s.png",
             covers_dir, g_channels.channels[chan_idx].id);

    g_cover_loading = 1;
    pthread_t t;
    pthread_create(&t, NULL, cover_dl_thread, arg);
    pthread_detach(t);
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
// WiFi – enable silently, auto-proceed on success
// ----------------------------------------------------------------

/* Shows a connecting screen (no user input), calls wifi_enable() synchronously.
   Returns true if WiFi is up.  On failure shows a brief error and returns false. */
static bool ensure_wifi(void) {
    if (wifi_is_active()) return true;

    /* Draw a minimal "connecting" screen – no OK button, auto-proceeds */
    {
        int      sw  = ap_get_screen_width();
        int      sh  = ap_get_screen_height();
        TTF_Font *fm = ap_get_font(AP_FONT_LARGE);
        ap_color black = {0, 0, 0, 255};
        ap_draw_rect(0, 0, sw, sh, black);
        ap_draw_status_bar(&g_status_bar);
        if (fm) {
            const char *msg = "Connecting to WiFi \xe2\x80\xa6";
            int w = ap_measure_text(fm, msg);
            int h = TTF_FontHeight(fm);
            ap_color c = {200, 200, 200, 255};
            ap_draw_text(fm, msg, (sw - w) / 2, (sh - h) / 2, c);
        }
        ap_present();
    }

    if (wifi_enable()) return true;

    show_message("", "Connect to WiFi");
    return false;
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
        "START       \xe2\x80\x94   Play / Stop (resume last)",
        "A           \xe2\x80\x94   Play selected station",
        "\xe2\x86\x91 / \xe2\x86\x93     \xe2\x80\x94   Navigate list",
        "\xe2\x86\x90 / \xe2\x86\x92     \xe2\x80\x94   Previous / Next station",
        "X           \xe2\x80\x94   Toggle favourite \xe2\x98\x85",
        "SELECT      \xe2\x80\x94   Sleep timer",
        "B           \xe2\x80\x94   Quit",
    };
    int hint_count = 7;

    /* Layout: heading + URL together at top, then gap, then key hints */
    int title_h = flg ? TTF_FontHeight(flg) + AP_S(6)  : AP_S(30);
    int url_h   = fsm ? TTF_FontHeight(fsm) + AP_S(20) : AP_S(28);  /* gap after URL */
    int line_h  = fsm ? TTF_FontHeight(fsm) + AP_S(10) : AP_S(28);
    int total_h = title_h + url_h + hint_count * line_h;

    int y     = (sh - total_h) / 2;
    int pad_x = AP_S(80);

    /* "Support SomaFM ♥" */
    if (flg) {
        ap_color yellow = {255, 220, 60, 255};
        ap_draw_text(flg, "Support SomaFM \xe2\x99\xa5", pad_x, y, yellow);
        y += title_h;
    }

    /* "somafm.com/support/" directly below heading */
    if (fsm) {
        ap_color grey = {140, 140, 140, 255};
        ap_draw_text(fsm, "somafm.com/support/", pad_x, y, grey);
        y += url_h;   /* includes the gap before hints */
    }

    /* Key hints */
    ap_color hint_col = {210, 210, 210, 255};
    for (int i = 0; i < hint_count; i++) {
        if (fsm) ap_draw_text(fsm, hints[i], pad_x, y, hint_col);
        y += line_h;
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
    int ft_h = ap_get_footer_height();  /* exact height from Apostrophe */
    int cont_top = sb_h;
    int cont_bot = sh  - ft_h;
    int cont_h   = cont_bot - cont_top;

    ap_theme *thm  = ap_get_theme();
    TTF_Font *fxsm = ap_get_font(AP_FONT_TINY);

    /* ── Black background ──────────────────────────────────────── */
    ap_color black = {0, 0, 0, 255};
    ap_draw_rect(0, 0, sw, sh, black);

    /* ── Cover art: right-aligned, full screen height ────────────
       Using sh (not cont_h) so the image extends behind the status bar
       and footer, giving a full-bleed cinematic background. */
    int cover_sz = sh;
    int cover_x  = sw - cover_sz;   /* flush against the right edge */

    if (g_cover_tex) {
        ap_draw_image(g_cover_tex, cover_x, 0, cover_sz, sh);
        /* Dim the cover when nothing is playing */
        if (g_playing_idx < 0) {
            ap_color dim = {0, 0, 0, 160};
            ap_draw_rect(cover_x, 0, cover_sz, sh, dim);
        }
    }

    /* ── Gradient: black on left, transparent on right ─────────── */
    /* Full screen height so the gradient also covers the status bar
       and footer areas, keeping text readable everywhere. */
    {
        int steps  = 48;
        int grad_w = cover_sz;
        for (int i = 0; i < steps; i++) {
            int   x = cover_x + (grad_w * i       / steps);
            int   w = cover_x + (grad_w * (i + 1) / steps) - x;
            if (w < 1) w = 1;
            Uint8 a = (Uint8)(255 - (255 * i / (steps - 1)));
            ap_color c = {0, 0, 0, a};
            ap_draw_rect(x, 0, w, sh, c);
        }
    }

    /* ── Status bar (battery / clock / wifi) ───────────────────── */
    ap_draw_status_bar(&g_status_bar);

    /* ── Stations list (left column) ────────────────────────────── */
    if (fxsm && g_channels.count > 0) {
        int list_pad_x = AP_S(12);
        /* Text may overlap the cover – limit only at the right screen edge */
        int list_maxw  = sw - list_pad_x - AP_S(8);
        int item_h     = TTF_FontHeight(fxsm) + AP_S(5);
        int visible    = cont_h / item_h;   /* how many items fit on screen */

        /* True centred-cursor scroll: cursor is always at position `half`.
           Items above and below wrap around the full channel list so the
           cursor never drifts from the centre of the screen. */
        int half = visible / 2;

        for (int i = 0; i < visible; i++) {
            /* Map screen row i to a channel index with full wrap-around */
            int ch_i = ((g_cursor - half + i) % g_channels.count
                        + g_channels.count) % g_channels.count;

            SomaChannel *ch      = &g_channels.channels[ch_i];
            bool         sel     = (ch_i == g_cursor);
            bool         playing = (ch_i == g_playing_idx);
            /* Show the grey highlight pill on the last-played station when stopped
               (paused indicator). Only when nothing is playing – once a new
               station starts the old paused highlight must disappear. */
            bool         paused  = (!playing && ch_i == g_last_played_idx
                                    && g_last_played_idx >= 0
                                    && g_playing_idx < 0);
            bool         fav     = favorites_contains(&g_favorites, ch->id);

            int iy = cont_top + i * item_h;

            /* Don't render items that would overlap the footer */
            if (iy + item_h > cont_bot) break;

            /* Draw gold ★ separately for favorites, then the rest of the label */
            int star_w = 0;
            if (fav && fxsm) {
                ap_color gold = {255, 200, 40, 255};
                ap_draw_text(fxsm, MARK_STAR, list_pad_x, iy, gold);
                star_w = ap_measure_text(fxsm, MARK_STAR);
            }
            int text_x = list_pad_x + star_w;

            /* "Station Name  ·  Genre" (ellipsized if too long) */
            char label[192] = "";
            strncat(label, ch->title, sizeof(label) - strlen(label) - 1);
            if (ch->genre[0]) {
                strncat(label, "  \xc2\xb7  ", sizeof(label) - strlen(label) - 1);
                strncat(label, ch->genre,      sizeof(label) - strlen(label) - 1);
            }

            /* Measure actual text width for pill (capped at list_maxw) */
            int tw       = ap_measure_text(fxsm, label) + star_w;
            if (tw > list_maxw) tw = list_maxw;
            int pill_pad = AP_S(8);
            int pill_x   = list_pad_x - pill_pad;
            int pill_w   = tw + 2 * pill_pad;
            int pill_r   = item_h / 2;

            /* Rounded highlight pill, sized to the text */
            if (sel) {
                ap_color white = {255, 255, 255, 230};
                ap_draw_rounded_rect(pill_x, iy, pill_w, item_h, pill_r, white);
            } else if (playing || paused) {
                ap_color play_bg = {180, 180, 180, 160};
                ap_draw_rounded_rect(pill_x, iy, pill_w, item_h, pill_r, play_bg);
            }

            /* Cursor → black on white, playing/paused → black on grey, otherwise → text */
            ap_color tc = sel            ? (ap_color){0,  0,  0, 255} :
                          (playing || paused) ? (ap_color){20, 20, 20, 255} :
                                    thm->text;
            ap_draw_text_ellipsized(fxsm, label, text_x, iy, tc, list_maxw - star_w);

            /* LIVE badge + listener count next to the playing station's pill */
            if (playing && fxsm) {
                int lw = ap_measure_text(fxsm, "LIVE");
                int lh = TTF_FontHeight(fxsm);
                int lp = AP_S(5);
                int lx = pill_x + pill_w + AP_S(6);
                int ly = iy + (item_h - lh) / 2;
                if (lx + lw + 2 * lp <= sw) {
                    ap_color live_bg = {200, 30, 30, 220};
                    ap_color live_fg = {255, 255, 255, 255};
                    ap_draw_rounded_rect(lx, ly - lp / 2, lw + 2 * lp, lh + lp, lh / 2, live_bg);
                    ap_draw_text(fxsm, "LIVE", lx + lp, ly, live_fg);
                }
            }
        }

    }


    /* ── Status bar and now-playing pill ───────────────────────── */
    ap_draw_status_bar(&g_status_bar);

    {
        char np_title[SOMA_NP_LEN]  = "";
        char np_artist[SOMA_NP_LEN] = "";
        pthread_mutex_lock(&g_np_mutex);
        memcpy(np_title,  g_np_title,  SOMA_NP_LEN);
        memcpy(np_artist, g_np_artist, SOMA_NP_LEN);
        pthread_mutex_unlock(&g_np_mutex);

        if (fxsm && (np_title[0] || np_artist[0])) {
            char np_buf[SOMA_NP_LEN * 2 + 32];
            if (np_artist[0] && np_title[0])
                snprintf(np_buf, sizeof(np_buf),
                         "\xe2\x99\xaa %s \xe2\x80\x93 %s", np_artist, np_title);
            else if (np_artist[0])
                snprintf(np_buf, sizeof(np_buf), "\xe2\x99\xaa %s", np_artist);
            else
                snprintf(np_buf, sizeof(np_buf), "\xe2\x99\xaa %s", np_title);

            if (g_playing_idx >= 0) {
                int lc = g_channels.channels[g_playing_idx].listeners;
                if (lc > 0) {
                    char lbuf[24];
                    if (lc >= 1000)
                        snprintf(lbuf, sizeof(lbuf), "  \xc2\xb7  %.1fk", lc / 1000.0);
                    else
                        snprintf(lbuf, sizeof(lbuf), "  \xc2\xb7  %d", lc);
                    strncat(np_buf, lbuf, sizeof(np_buf) - strlen(np_buf) - 1);
                }
            }

            int text_w = ap_measure_text(fxsm, np_buf);
            int text_h = TTF_FontHeight(fxsm);
            int tx  = AP_S(12);
            int ty  = (sb_h - text_h) / 2;
            int pad = AP_S(6);
            int pill_maxw = sw * 2 / 3;
            ap_color np_bg = {40, 40, 40, 180};
            ap_color np_fg = {220, 220, 220, 255};
            int pill_h = text_h + pad;
            int pill_r = pill_h / 2;

            if (text_w <= pill_maxw) {
                ap_draw_rounded_rect(tx - pad, ty - pad / 2, text_w + 2 * pad, pill_h, pill_r, np_bg);
                ap_draw_text(fxsm, np_buf, tx, ty, np_fg);
            } else {
                /* Apostrophe's ap_present() idles via SDL_WaitEventTimeout
                   when no frame is requested, to save power — it only
                   wakes on input or ~1s ticks. Without this, the ticker
                   would advance in one big jump per wake instead of
                   scrolling continuously. Schedule a wake every ~33ms
                   (30fps) instead of forcing full 60fps — still smooth
                   at this scroll speed (~3px/frame) but roughly halves
                   the redraw load while the marquee is active. */
                ap_request_frame_in(33);

                if (strcmp(np_buf, g_ticker_last) != 0) {
                    strncpy(g_ticker_last, np_buf, sizeof(g_ticker_last) - 1);
                    g_ticker_px = 0;
                    g_ticker_ms = 0;
                }
                if (g_ticker_ms > 2000)
                    g_ticker_px = (int)((g_ticker_ms - 2000) * 90 / 1000);

                ap_draw_rounded_rect(tx - pad, ty - pad / 2, pill_maxw + 2 * pad, pill_h, pill_r, np_bg);

                const char *sep   = "   ***   ";
                int          sep_w = ap_measure_text(fxsm, sep);
                int          full_w = text_w + sep_w;
                if (g_ticker_px > full_w) { g_ticker_ms = 0; g_ticker_px = 0; }

                SDL_Renderer *rend = ap_get_renderer();
                SDL_Rect clip = { tx - pad, 0, pill_maxw + 2 * pad, sb_h };
                SDL_RenderSetClipRect(rend, &clip);
                ap_color sep_col = {120, 120, 120, 255};
                ap_draw_text(fxsm, np_buf, tx - g_ticker_px,           ty, np_fg);
                ap_draw_text(fxsm, sep,    tx - g_ticker_px + text_w,  ty, sep_col);
                ap_draw_text(fxsm, np_buf, tx - g_ticker_px + full_w,  ty, np_fg);
                SDL_RenderSetClipRect(rend, NULL);
            }
        }
    }

    /* ── Gradual dim overlay (F) ────────────────────────────────── */
    {
        uint8_t dim = screen_dim_alpha();
        if (dim > 0)
            ap_draw_rect(0, 0, sw, sh, (ap_color){0, 0, 0, dim});
    }

    /* ── Footer ─────────────────────────────────────────────────── */
    /* START is the central Play/Stop action; A selects/switches station. */
    {
        ap_footer_item footer[] = {
            { .button = AP_BTN_MENU, .label = "Key Hints" },
        };
        ap_draw_footer(footer, 1);
    }

    /* ── Channel description pill (bottom-right inside footer) ──── */
    /* Visible for 10 s after the last navigation input, then hides.
       Shows up to 2 wrapped lines; SDL clip rect prevents overflow. */
    if (fxsm && g_cursor >= 0 && g_cursor < g_channels.count
        && SDL_GetTicks() < g_desc_hide_at) {
        const char *desc = g_channels.channels[g_cursor].description;
        if (desc && desc[0]) {
            int pad       = AP_S(8);
            int text_h    = TTF_FontHeight(fxsm);
            int line_skip = TTF_FontLineSkip(fxsm);
            int two_line_h = text_h + line_skip;
            int pill_h    = two_line_h + pad;
            int pill_r    = pill_h / 2;   /* true pill shape */
            int max_w     = sw / 2;
            int pill_w    = max_w + 2 * pad;
            int pill_x    = sw - pill_w - AP_S(12);
            int pill_y    = sh - ft_h + (ft_h - pill_h) / 2;

            ap_color bg  = {40, 40, 40, 180};
            ap_color fg  = {200, 200, 200, 255};
            ap_draw_rounded_rect(pill_x, pill_y, pill_w, pill_h, pill_r, bg);

            SDL_Renderer *rend = ap_get_renderer();
            SDL_Rect clip = { pill_x + pad, pill_y + pad / 2,
                              max_w, two_line_h };
            SDL_RenderSetClipRect(rend, &clip);
            ap_draw_text_wrapped(fxsm, desc,
                                 pill_x + pad, pill_y + pad / 2,
                                 max_w, fg, AP_ALIGN_LEFT);
            SDL_RenderSetClipRect(rend, NULL);
        }
    }
}

static void show_sleep_timer(void);   /* defined below */

// ----------------------------------------------------------------
// Main screen loop – the only screen in the app
// ----------------------------------------------------------------
static void screen_main(void) {
    bool     show_hint       = false;
    uint32_t last_ticks      = SDL_GetTicks();
    uint32_t suspend_idle_ms = 0;   /* how long screen has been off with no stream */

    /* Show description pill immediately on launch for 10 s */
    g_desc_hide_at = SDL_GetTicks() + 10000;

    /* Pre-load the cover for the initial cursor position so something
       is visible right away when the user opens the app. */
    if (g_cursor >= 0 && g_cursor < g_channels.count)
        load_cover(g_cursor);

    while (1) {
        /* ── Screen timeout timer ────────────────────────────────── */
        uint32_t now_ticks = SDL_GetTicks();
        uint32_t elapsed   = now_ticks - last_ticks;
        screen_update(elapsed);
        last_ticks = now_ticks;

        /* ── Audio sink hotplug ──────────────────────────────────── */
        /* USB-C interface / Bluetooth speaker plugged in or removed —
         * rejoin the stream so mpg123 picks up the new device. */
        player_check_audio_sink();

        /* ── Ticker time ─────────────────────────────────────────── */
        if (!screen_is_off()) g_ticker_ms += elapsed;

        /* ── Sleep timer ─────────────────────────────────────────── */
        if (g_sleep_timer_ms > 0) {
            if (elapsed >= g_sleep_timer_ms) {
                g_sleep_timer_ms = 0;
                player_stop(); np_stop(); g_playing_idx = -1;
            } else {
                g_sleep_timer_ms -= elapsed;
            }
        }

        /* ── Auto-suspend (screen off + no stream) ───────────────── */
        /* Count how long both conditions are true simultaneously.
           As soon as either changes (screen on, or stream starts), reset. */
        if (screen_is_off() && g_playing_idx < 0)
            suspend_idle_ms += elapsed;
        else
            suspend_idle_ms = 0;

        if (g_suspend_timeout_ms > 0 && suspend_idle_ms >= g_suspend_timeout_ms) {
            /* Conditions met — build the suspend path from env vars so it
               works regardless of where NextUI is installed. */
            const char *sdcard   = getenv("SDCARD_PATH");
            const char *platform = getenv("PLATFORM");
            if (!sdcard   || !sdcard[0])   sdcard   = "/mnt/SDCARD";
            if (!platform || !platform[0]) platform = "tg5040";
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "%s/.system/%s/bin/suspend", sdcard, platform);

            /* Stop audio first so ALSA is released before the kernel suspends.
               Without this the kernel may retry S2RAM up to 5× with 3 s gaps. */
            if (g_playing_idx >= 0) {
                player_stop(); np_stop(); g_playing_idx = -1;
            }

            system(cmd);   /* blocks until device wakes up */

            /* SDL_GetTicks() may reflect wall time after wakeup; reset the
               frame-delta base so the first post-wakeup frame doesn't see
               a huge elapsed value that would immediately re-trigger suspend. */
            last_ticks      = SDL_GetTicks();
            suspend_idle_ms = 0;

            /* Bring the screen back on after wakeup. */
            screen_on();

            /* Restart the last stream so playback resumes automatically. */
            if (g_last_played_idx >= 0) {
                if (play_channel(g_last_played_idx))
                    load_cover(g_last_played_idx);
            }
        }

        /* ── Async cover: create texture when download is done ───── */
        /* SDL textures must be created on the main thread.
           The download thread saves the file and sets g_cover_dl_done. */
        if (g_cover_dl_done) {
            g_cover_dl_done = 0;
            if (g_cover_dl_idx == g_cover_chan_idx) {
                if (g_cover_tex) { SDL_DestroyTexture(g_cover_tex); g_cover_tex = NULL; }
                g_cover_tex   = ap_load_image(g_cover_dl_path);
            }
        }

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
                    /* Wrap from first item to last (Endlosscrollen) */
                    g_cursor = (g_cursor > 0) ? g_cursor - 1 : g_channels.count - 1;
                    g_desc_hide_at = SDL_GetTicks() + 10000;
                    break;

                case AP_BTN_DOWN:
                    /* Wrap from last item to first (Endlosscrollen) */
                    g_cursor = (g_cursor < g_channels.count - 1) ? g_cursor + 1 : 0;
                    g_desc_hide_at = SDL_GetTicks() + 10000;
                    break;

                case AP_BTN_LEFT: {
                    /* Previous station with wrap, relative to what's actually
                       playing (not wherever the list cursor happens to be) —
                       so repeated LEFT/RIGHT presses step through stations in
                       order even if the user scrolled the list in between. */
                    int base = (g_playing_idx >= 0) ? g_playing_idx : g_cursor;
                    g_cursor = (base > 0) ? base - 1 : g_channels.count - 1;
                    g_desc_hide_at = SDL_GetTicks() + 10000;
                    if (play_channel(g_cursor)) load_cover(g_cursor);
                    break;
                }

                case AP_BTN_RIGHT: {
                    /* Next station with wrap, relative to what's actually
                       playing — see AP_BTN_LEFT comment above. */
                    int base = (g_playing_idx >= 0) ? g_playing_idx : g_cursor;
                    g_cursor = (base < g_channels.count - 1) ? base + 1 : 0;
                    g_desc_hide_at = SDL_GetTicks() + 10000;
                    if (play_channel(g_cursor)) load_cover(g_cursor);
                    break;
                }

                case AP_BTN_A:
                    /* A always plays / switches to the highlighted station */
                    if (play_channel(g_cursor))
                        load_cover(g_cursor);
                    break;

                case AP_BTN_START:
                    /* START toggles the last-played station (stop ↔ resume).
                       Does not switch to a different station — use A for that. */
                    if (g_playing_idx >= 0) {
                        g_last_played_idx = g_playing_idx;
                        player_stop();
                        np_stop();
                        g_playing_idx = -1;
                    } else {
                        /* Resume last played station, or fall back to cursor */
                        int resume = (g_last_played_idx >= 0) ? g_last_played_idx : g_cursor;
                        if (play_channel(resume)) load_cover(resume);
                    }
                    break;

                case AP_BTN_X:
                    /* Toggle favourite for the highlighted station */
                    if (g_cursor >= 0 && g_cursor < g_channels.count)
                        favorites_toggle(&g_favorites, g_channels.channels[g_cursor].id);
                    break;

                case AP_BTN_SELECT:
                    show_sleep_timer();
                    screen_activity();
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
// Sleep timer
// ----------------------------------------------------------------

static void show_sleep_timer(void) {
    static const char *labels[] = { "Off", "15 minutes", "30 minutes",
                                    "60 minutes", "90 minutes" };
    static const uint32_t values[] = { 0, 15*60000, 30*60000, 60*60000, 90*60000 };
    int n = 5;

    ap_list_item items[5];
    char trailing[5][16];
    for (int i = 0; i < n; i++) {
        snprintf(trailing[i], sizeof(trailing[i]), "%s",
                 (g_sleep_timer_ms == values[i]) ? "\xe2\x9c\x93" : "");
        items[i] = (ap_list_item){ .label = labels[i],
                                   .trailing_text = trailing[i][0] ? trailing[i] : NULL };
    }

    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Cancel" },
        { .button = AP_BTN_A, .label = "Set", .is_confirm = true },
    };
    ap_list_opts opts = ap_list_default_opts("Sleep Timer", items, n);
    opts.footer       = footer;
    opts.footer_count = 2;
    opts.status_bar   = &g_status_bar;

    ap_list_result result;
    if (ap_list(&opts, &result) != AP_CANCELLED)
        g_sleep_timer_ms = values[result.selected_index];
}

// ----------------------------------------------------------------
// NextUI settings integration (minuisettings.txt)
// ----------------------------------------------------------------

/* Read screentimeout and suspendTimeout from
   $SHARED_USERDATA_PATH/minuisettings.txt (key=value, values in seconds).
   Outputs milliseconds; 0 means "never / disabled".
   Falls back to 5 min screen timeout and suspend disabled. */
static void load_minui_settings(uint32_t *screen_ms, uint32_t *suspend_ms) {
    *screen_ms  = 300000;   /* 5 min fallback */
    *suspend_ms = 0;        /* suspend disabled by default */

    const char *base = getenv("SHARED_USERDATA_PATH");
    if (!base || !base[0]) base = "/mnt/SDCARD/.userdata/shared";
    char path[300];
    snprintf(path, sizeof(path), "%s/minuisettings.txt", base);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char key[64]; int val;
    while (fscanf(f, " %63[^=]=%d", key, &val) == 2) {
        if (strcmp(key, "screentimeout") == 0)
            *screen_ms  = (val <= 0) ? 0 : (uint32_t)val * 1000;
        else if (strcmp(key, "suspendTimeout") == 0)
            *suspend_ms = (val <= 0) ? 0 : (uint32_t)val * 1000;
    }
    fclose(f);
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
    {
        uint32_t screen_ms, suspend_ms;
        load_minui_settings(&screen_ms, &suspend_ms);
        screen_set_timeout(screen_ms);
        g_suspend_timeout_ms = suspend_ms;
    }
    screen_init();
    screen_init_power();

    soma_init();
    player_init();
    favorites_load(&g_favorites);

    /* Build the cache file path once, reused by the background thread */
    snprintf(g_channels_cache_path, sizeof(g_channels_cache_path),
             "%s/channels_cache.json", userdata_dir());

    /* Enable WiFi silently (auto-proceeds on success, error on failure) */
    if (!ensure_wifi()) goto cleanup;

    /* Fast path: load channel list from the on-disk cache written by a
       previous session.  If the cache is missing or corrupt (first ever
       launch, or manually deleted), fall back to a synchronous network
       fetch and write the result as the new cache so future launches
       are fast.  Either way, a background thread then fetches a fresh
       copy from the API so the *next* launch sees up-to-date data. */
    if (!soma_channels_cache_load(g_channels_cache_path, &g_channels)
        || g_channels.count == 0) {
        /* First launch or corrupt cache — fetch synchronously */
        if (!soma_fetch_channels(&g_channels) || g_channels.count == 0) {
            show_message("Network Error",
                "Could not load station list.\nCheck your internet connection.");
            goto cleanup;
        }
        /* Persist what we just fetched so the next launch hits the fast path */
        soma_channels_cache_refresh(g_channels_cache_path);
    } else {
        /* Cache loaded — refresh in the background for the next session */
        start_cache_refresh();
    }

    /* Start at last played station and auto-play it */
    {
        int last = load_last_station();
        g_cursor = (last >= 0 && last < g_channels.count) ? last : 0;
        play_channel(g_cursor);   /* auto-play on launch */
    }

    screen_main();

cleanup:
    player_cleanup();
    soma_cleanup();
    screen_quit_power();
    ap_quit();
    return 0;
}
