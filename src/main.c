// ================================================================
// main.c  –  SomaFM Radio Player for TrimUI / NextUI
//
// Main menu:
//   Stations   – all channels, X = toggle favorite
//   Favorites  – saved favorites, X = remove
//
// Controls (both lists):
//   ↑ / ↓      → Navigate
//   A           → Play / Now Playing
//   X           → Toggle / Remove favorite
//   Y           → Stop playback (only when playing)
//   MENU        → Settings (screen timeout)
//   B / MENU    → Back / Exit
//
// Controls (Now Playing):
//   A           → Stop
//   B           → Back to list (music keeps playing)
//   SELECT + A  → Wake screen (when screen is off)
// ================================================================

#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_WIDGETS_IMPLEMENTATION
#include "apostrophe_widgets.h"

#include "soma.h"
#include "player.h"
#include "wifi.h"
#include "favorites.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

// ----------------------------------------------------------------
// Screen-state constants (used in the Now Playing loop)
// ----------------------------------------------------------------
#define SCREEN_ON   0
#define SCREEN_OFF  1
#define SCREEN_HINT 2   /* "Select+A" hint shown for 1 s */

// ----------------------------------------------------------------
// Backlight control – TG5040 only
// Identical to nexttimer implementation.
// ----------------------------------------------------------------
#ifdef PLATFORM_TG5040
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define DISP_LCD_SET_BRIGHTNESS 0x102
#define SHM_KEY "/SharedSettings"

typedef struct { int version; int brightness; } MinSettings;

static void backlight_raw(int val) {
    int fd = open("/dev/disp", O_RDWR);
    if (fd >= 0) {
        unsigned long param[4] = {0, (unsigned long)val, 0, 0};
        ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
        close(fd);
    }
}

static int backlight_read_level(void) {
    int fd = shm_open(SHM_KEY, O_RDWR, 0644);
    if (fd < 0) return 7;
    MinSettings *s = mmap(NULL, sizeof(MinSettings),
                          PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) return 7;
    int level = s->brightness;
    munmap(s, sizeof(MinSettings));
    return (level >= 0 && level <= 10) ? level : 7;
}

static void backlight_off(void) { backlight_raw(0); }
static void backlight_on(void) {
    static const int tbl[11] = {1,8,16,32,48,72,96,128,160,192,255};
    int level = backlight_read_level();
    backlight_raw(8);
    backlight_raw(tbl[level]);
}
#endif /* PLATFORM_TG5040 */

// ----------------------------------------------------------------
// Settings
// ----------------------------------------------------------------
typedef struct {
    int screen_timeout;   /* seconds; 0 = never */
} Settings;

/* Shared userdata directory: $SHARED_USERDATA_PATH/somaplayer/ */
static const char *userdata_dir(void) {
    static char dir[256];
    if (dir[0]) return dir;
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (!base || !base[0]) base = "/mnt/SDCARD/.userdata/shared";
    snprintf(dir, sizeof(dir), "%s/somaplayer", base);
    return dir;   /* favorites.c already calls mkdir; no need to repeat here */
}

static void settings_init(Settings *s) {
    s->screen_timeout = 60;
}

static void settings_load(Settings *s) {
    settings_init(s);
    char path[280];
    snprintf(path, sizeof(path), "%s/settings.txt", userdata_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char key[32]; int val;
    while (fscanf(f, " %31[^=]=%d", key, &val) == 2)
        if (strcmp(key, "screen_timeout") == 0) s->screen_timeout = val;
    fclose(f);
}

static void settings_save(const Settings *s) {
    char path[280];
    snprintf(path, sizeof(path), "%s/settings.txt", userdata_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "screen_timeout=%d\n", s->screen_timeout);
    fclose(f);
}

// ----------------------------------------------------------------
// Screen-timeout helpers
// ----------------------------------------------------------------
static const int   TIMEOUT_VALS[]   = {0, 10, 30, 60, 120, 300};
static const char *TIMEOUT_LABELS[] = {"Never","10 sec","30 sec",
                                        "1 min","2 min","5 min"};
#define TIMEOUT_COUNT 6



// ----------------------------------------------------------------
// Global state
// ----------------------------------------------------------------
static SomaChannelList g_channels    = {0};
static int             g_playing_idx = -1;
static FavoriteList    g_favorites   = {0};
static Settings        g_settings;

static ap_status_bar_opts g_status_bar = {
    .show_clock   = AP_CLOCK_AUTO,
    .show_battery = true,
    .show_wifi    = true,
};

/* Cover art */
static SDL_Texture *g_cover_tex       = NULL;
static int          g_cover_chan_idx  = -2;  /* -2 = none loaded */

/* Now-playing metadata (background polling thread) */
static volatile int    g_np_running  = 0;
static pthread_mutex_t g_np_mutex    = PTHREAD_MUTEX_INITIALIZER;
static char            g_np_title[SOMA_NP_LEN]  = "";
static char            g_np_artist[SOMA_NP_LEN] = "";
static int             g_np_for_idx = -1;

static void *np_poll_thread(void *arg) {
    (void)arg;
    int first = 1;
    while (g_np_running) {
        if (!first) {
            for (int i = 0; i < 30 && g_np_running; i++)
                sleep(1);
        }
        first = 0;
        if (!g_np_running) break;

        pthread_mutex_lock(&g_np_mutex);
        int idx = g_np_for_idx;
        pthread_mutex_unlock(&g_np_mutex);

        if (idx < 0 || idx >= g_channels.count) continue;

        char title[SOMA_NP_LEN]  = "";
        char artist[SOMA_NP_LEN] = "";
        soma_fetch_now_playing(g_channels.channels[idx].id, title, artist);

        pthread_mutex_lock(&g_np_mutex);
        if (g_np_for_idx == idx) {
            memcpy(g_np_title,  title,  SOMA_NP_LEN);
            memcpy(g_np_artist, artist, SOMA_NP_LEN);
        }
        pthread_mutex_unlock(&g_np_mutex);
    }
    return NULL;
}

static void np_start(int chan_idx) {
    pthread_mutex_lock(&g_np_mutex);
    g_np_for_idx = chan_idx;
    g_np_title[0]  = '\0';
    g_np_artist[0] = '\0';
    pthread_mutex_unlock(&g_np_mutex);

    if (!g_np_running) {
        g_np_running = 1;
        pthread_t t;
        pthread_create(&t, NULL, np_poll_thread, NULL);
        pthread_detach(t);
    }
}

static void np_stop(void) {
    g_np_running = 0;
    pthread_mutex_lock(&g_np_mutex);
    g_np_for_idx   = -1;
    g_np_title[0]  = '\0';
    g_np_artist[0] = '\0';
    pthread_mutex_unlock(&g_np_mutex);
}

/* UTF-8 literals used in trailing text / labels */
#define MARK_PLAY "\xe2\x96\xb6 "   /* ▶  */
#define MARK_STAR "\xe2\x98\x85 "   /* ★  */

// ----------------------------------------------------------------
// Helper: simple info / error message
// ----------------------------------------------------------------
// ----------------------------------------------------------------
// Cover art download + cache
// ----------------------------------------------------------------
static size_t cover_write_cb(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    return fwrite(ptr, size, nmemb, fp);
}

static void load_cover(int chan_idx) {
    if (chan_idx == g_cover_chan_idx) return;
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
    curl_easy_setopt(curl, CURLOPT_CAINFO, "./cacert.pem");
#endif
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res == CURLE_OK)
        g_cover_tex = ap_load_image(tmp);
}

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
// Helper: start playback for channel idx.
// Returns 1 on success, 0 on error (message already shown).
// ----------------------------------------------------------------
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
    return 1;
}

// ----------------------------------------------------------------
// Settings screen (opened by MENU button)
// ----------------------------------------------------------------
// ----------------------------------------------------------------
// Settings – ap_options_list widget
// Opened from custom loops (main menu, now playing) so MENU works.
// ----------------------------------------------------------------
static void screen_settings(void) {
    ap_option timeout_opts[TIMEOUT_COUNT];
    int cur_sel = 0;
    for (int i = 0; i < TIMEOUT_COUNT; i++) {
        timeout_opts[i].label = TIMEOUT_LABELS[i];
        timeout_opts[i].value = TIMEOUT_LABELS[i];
        if (TIMEOUT_VALS[i] == g_settings.screen_timeout) cur_sel = i;
    }

    ap_options_item items[] = {
        {
            .label           = "Screen Timeout",
            .type            = AP_OPT_STANDARD,
            .options         = timeout_opts,
            .option_count    = TIMEOUT_COUNT,
            .selected_option = cur_sel,
        },
        {
            .label        = "Support SomaFM \xe2\x99\xa5",
            .type         = AP_OPT_STANDARD,
            .option_count = 0,
        },
        {
            .label        = "  somafm.com/support/",
            .type         = AP_OPT_STANDARD,
            .option_count = 0,
        },
    };

    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
    };

    ap_options_list_opts opts = {
        .title        = "Settings",
        .items        = items,
        .item_count   = 3,
        .footer       = footer,
        .footer_count = 1,
        .status_bar   = &g_status_bar,
    };

    ap_options_list_result result;
    ap_options_list(&opts, &result);

    g_settings.screen_timeout = TIMEOUT_VALS[items[0].selected_option];
    settings_save(&g_settings);
}

// ----------------------------------------------------------------
// Now Playing – custom render helpers
// ----------------------------------------------------------------
static void render_screen_off(int show_hint) {
    int      sw  = ap_get_screen_width();
    int      sh  = ap_get_screen_height();
    ap_color blk = {0, 0, 0, 255};
    ap_draw_rect(0, 0, sw, sh, blk);

    if (show_hint) {
        ap_theme  *thm  = ap_get_theme();
        TTF_Font  *fmed = ap_get_font(AP_FONT_MEDIUM);
        TTF_Font  *fsm  = ap_get_font(AP_FONT_SMALL);
        if (fmed) {
            const char *l1 = "Press Select + A";
            ap_draw_text(fmed, l1,
                (sw - ap_measure_text(fmed, l1)) / 2,
                sh / 2 - TTF_FontHeight(fmed) - AP_S(4), thm->text);
        }
        if (fsm) {
            const char *l2 = "to wake the screen";
            ap_draw_text(fsm, l2,
                (sw - ap_measure_text(fsm, l2)) / 2,
                sh / 2 + AP_S(4), thm->hint);
        }
    }
    ap_present();
}

static void render_now_playing(int idx) {
    SomaChannel  *ch  = &g_channels.channels[idx];
    PlayerStatus  st  = player_status();

    int      sw   = ap_get_screen_width();
    int      sh   = ap_get_screen_height();
    int      sb_h = ap_get_status_bar_height();
    ap_theme *thm = ap_get_theme();

    TTF_Font *flg  = ap_get_font(AP_FONT_LARGE);
    TTF_Font *fsm  = ap_get_font(AP_FONT_SMALL);
    TTF_Font *fxsm = ap_get_font(AP_FONT_TINY);

    ap_draw_background();
    ap_draw_status_bar(&g_status_bar);

    /* Layout */
    int pad      = AP_S(16);
    int cont_top = sb_h + pad;
    int cont_bot = sh - AP_S(44);
    int cont_h   = cont_bot - cont_top;

    /* Cover art – centered above text */
    int cover_sz = 0;
    int y        = cont_top;

    if (g_cover_tex) {
        cover_sz = AP_S(220);
        if (cover_sz > cont_h / 2) cover_sz = cont_h / 2;
        ap_draw_image(g_cover_tex, pad, y, cover_sz, cover_sz);
        y += cover_sz + AP_S(12);
    }

    /* Status icon + title */
    char title_buf[96];
    const char *icon = (st == PLAYER_PLAYING) ? "\xe2\x97\x8f " :
                       (st == PLAYER_ERROR)   ? "\xe2\x9c\x95 " :
                                                "\xe2\x96\xa0 ";
    snprintf(title_buf, sizeof(title_buf), "%s%s", icon, ch->title);

    /* Genre · listeners */
    char info_buf[128] = "";
    if (ch->genre[0]) strncat(info_buf, ch->genre, 60);
    if (ch->listeners > 0) {
        char lbuf[24];
        snprintf(lbuf, sizeof(lbuf), " \xc2\xb7 %d", ch->listeners);
        strncat(info_buf, lbuf, sizeof(info_buf) - strlen(info_buf) - 1);
    }

    /* Read now-playing metadata (written by background thread) */
    char np_title[SOMA_NP_LEN]  = "";
    char np_artist[SOMA_NP_LEN] = "";
    pthread_mutex_lock(&g_np_mutex);
    memcpy(np_title,  g_np_title,  SOMA_NP_LEN);
    memcpy(np_artist, g_np_artist, SOMA_NP_LEN);
    pthread_mutex_unlock(&g_np_mutex);
    int has_np = np_title[0] || np_artist[0];

    int title_h = flg  ? TTF_FontHeight(flg)  : 0;
    int info_h  = fsm  ? TTF_FontHeight(fsm)   : 0;
    int desc_h  = fxsm && ch->description[0] ? TTF_FontHeight(fxsm) : 0;
    int text_maxw = sw - 2 * pad;

    /* When no cover: vertically center the text block */
    if (!g_cover_tex) {
        int total_h = title_h
                    + (info_buf[0]        ? AP_S(6)  + info_h  : 0)
                    + (ch->description[0] ? AP_S(10) + desc_h  : 0)
                    + (has_np             ? AP_S(10) + info_h  : 0);
        y = cont_top + (cont_h - total_h) / 2;
        if (y < cont_top) y = cont_top;
    }

    if (flg) {
        ap_color lightgray = {210, 210, 210, 255};
        ap_draw_text_ellipsized(flg, title_buf, pad, y, lightgray, text_maxw);
        y += title_h;
    }
    if (fsm && info_buf[0]) {
        y += AP_S(6);
        ap_draw_text_ellipsized(fsm, info_buf, pad, y, thm->hint, text_maxw);
        y += info_h;
    }
    if (fxsm && ch->description[0]) {
        y += AP_S(10);
        ap_draw_text_ellipsized(fxsm, ch->description, pad, y, thm->text, text_maxw);
        y += desc_h;
    }
    if (fsm && has_np) {
        char np_buf[SOMA_NP_LEN * 2 + 8];
        if (np_artist[0] && np_title[0])
            snprintf(np_buf, sizeof(np_buf),
                     "\xe2\x99\xaa %s \xe2\x80\x93 %s", np_artist, np_title);
        else if (np_artist[0])
            snprintf(np_buf, sizeof(np_buf), "\xe2\x99\xaa %s", np_artist);
        else
            snprintf(np_buf, sizeof(np_buf), "\xe2\x99\xaa %s", np_title);
        y += AP_S(10);
        ap_color np_gray = {210, 210, 210, 255};
        ap_draw_text_ellipsized(fsm, np_buf, pad, y, np_gray, text_maxw);
    }

    ap_footer_item footer[] = {
        { .button = AP_BTN_B,    .label = "Back"                       },
        { .button = AP_BTN_MENU, .label = "Settings"                   },
        { .button = AP_BTN_A,    .label = "Stop", .is_confirm = true   },
    };
    ap_draw_footer(footer, 3);

    ap_present();
}

// ----------------------------------------------------------------
// Now Playing screen – custom loop with screen timeout
// ----------------------------------------------------------------
static void screen_now_playing(int idx) {
    int      scr         = SCREEN_ON;
    uint32_t last_input  = SDL_GetTicks();
    uint32_t hint_start  = 0;
    int      select_held = 0;

    while (1) {
        /* ── Input ───────────────────────────────────────────── */
        ap_input_event ev;
        while (ap_poll_input(&ev)) {
            if (ev.button == AP_BTN_SELECT)
                select_held = ev.pressed;

            if (!ev.pressed) continue;

            if (scr != SCREEN_ON) {
                /* Screen is off: SELECT+A wakes, anything else shows hint */
                if (ev.button == AP_BTN_A && select_held) {
                    scr       = SCREEN_ON;
                    last_input = SDL_GetTicks();

#ifdef PLATFORM_TG5040
                    backlight_on();
#endif
                } else if (scr == SCREEN_OFF) {
                    scr        = SCREEN_HINT;
                    hint_start = SDL_GetTicks();
#ifdef PLATFORM_TG5040
                    backlight_on();
#endif
                }
                continue;
            }

            last_input = SDL_GetTicks();

            if (ev.button == AP_BTN_SELECT) {
                scr = SCREEN_OFF;

#ifdef PLATFORM_TG5040
                backlight_off();
#endif
                continue;
            }
            if (ev.button == AP_BTN_B) return;
            if (ev.button == AP_BTN_MENU) { screen_settings(); last_input = SDL_GetTicks(); continue; }
            if (ev.button == AP_BTN_A) {
                player_stop();
                np_stop();
                g_playing_idx = -1;
                return;
            }
        }

        /* ── Screen-timeout logic ────────────────────────────── */
        uint32_t now    = SDL_GetTicks();
        int      was_on = (scr == SCREEN_ON);

        if (scr == SCREEN_ON && g_settings.screen_timeout > 0
                && (now - last_input) >= (uint32_t)(g_settings.screen_timeout * 1000)) {
            scr = SCREEN_OFF;
        }
        if (scr == SCREEN_HINT && (now - hint_start) >= 1000u) {
            scr = SCREEN_OFF;
#ifdef PLATFORM_TG5040
            backlight_off();
#endif
        }

        if (was_on && scr != SCREEN_ON) {
#ifdef PLATFORM_TG5040
            backlight_off();
#endif
        } else if (!was_on && scr == SCREEN_ON) {
#ifdef PLATFORM_TG5040
            backlight_on();
#endif
        }

        /* ── Render ──────────────────────────────────────────── */
        if (scr != SCREEN_ON) {
            render_screen_off(scr == SCREEN_HINT);
            SDL_Delay(50);
        } else {
            render_now_playing(idx);
            SDL_Delay(33);   /* ~30 fps */
        }
    }
}

// ----------------------------------------------------------------
// Stations screen  –  all channels, X = toggle favorite
// Returns 0 to go back to main menu, 1 to loop within screen.
// ----------------------------------------------------------------
static int screen_stations(void) {
    if (g_channels.count == 0) return 0;

    static ap_list_item items[SOMA_MAX_CHANNELS];
    static char         trailing[SOMA_MAX_CHANNELS][80];

    for (int i = 0; i < g_channels.count; i++) {
        SomaChannel *ch  = &g_channels.channels[i];
        bool is_playing  = (i == g_playing_idx);
        bool is_fav      = favorites_contains(&g_favorites, ch->id);

        trailing[i][0] = '\0';
        if (is_playing) strcat(trailing[i], MARK_PLAY);
        if (is_fav)     strcat(trailing[i], MARK_STAR);
        if (ch->genre[0]) strncat(trailing[i], ch->genre, 40);

        items[i] = (ap_list_item){
            .label        = ch->title,
            .trailing_text = trailing[i][0] ? trailing[i] : NULL,
        };
    }

    ap_footer_item footer_base[] = {
        { .button = AP_BTN_B,    .label = "Back"                           },
        { .button = AP_BTN_X,    .label = "\xe2\x98\x85"                  },
        { .button = AP_BTN_A,    .label = "Play", .is_confirm = true       },
    };
    ap_footer_item footer_stop[] = {
        { .button = AP_BTN_B,    .label = "Back"                           },
        { .button = AP_BTN_X,    .label = "\xe2\x98\x85"                  },
        { .button = AP_BTN_Y,    .label = "Stop"                           },
        { .button = AP_BTN_A,    .label = "Play", .is_confirm = true       },
    };

    ap_list_opts opts = ap_list_default_opts("Stations", items, g_channels.count);
    opts.status_bar    = &g_status_bar;
    opts.action_button = AP_BTN_X;
    opts.initial_index = (g_playing_idx >= 0) ? g_playing_idx : 0;

    if (g_playing_idx >= 0) {
        opts.footer                  = footer_stop;
        opts.footer_count            = 4;
        opts.secondary_action_button = AP_BTN_Y;
    } else {
        opts.footer       = footer_base;
        opts.footer_count = 3;
    }

    ap_list_result result;
    int rc = ap_list(&opts, &result);

    if (rc == AP_CANCELLED) return 0;

    if (result.action == AP_ACTION_TRIGGERED) {
        favorites_toggle(&g_favorites, g_channels.channels[result.selected_index].id);
        return 1;
    }
    if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
        player_stop();
        np_stop();
        g_playing_idx = -1;
        return 1;
    }

    int idx = result.selected_index;
    if (idx == g_playing_idx) { screen_now_playing(idx); return 1; }
    if (play_channel(idx)) { load_cover(idx); screen_now_playing(idx); }
    return 1;
}

// ----------------------------------------------------------------
// Favorites screen  –  saved favorites, X = remove
// Returns 0 to go back to main menu, 1 to loop within screen.
// ----------------------------------------------------------------
static int screen_favorites(void) {
    static ap_list_item fav_items[SOMA_MAX_CHANNELS];
    static int          fav_chan[SOMA_MAX_CHANNELS];
    static char         fav_trailing[SOMA_MAX_CHANNELS][64];
    int fav_count = 0;

    for (int i = 0; i < g_channels.count; i++) {
        if (!favorites_contains(&g_favorites, g_channels.channels[i].id)) continue;
        SomaChannel *ch = &g_channels.channels[i];
        bool is_playing = (i == g_playing_idx);

        fav_trailing[fav_count][0] = '\0';
        if (is_playing) strcat(fav_trailing[fav_count], MARK_PLAY);
        if (ch->genre[0]) strncat(fav_trailing[fav_count], ch->genre, 40);

        fav_items[fav_count] = (ap_list_item){
            .label        = ch->title,
            .trailing_text = fav_trailing[fav_count][0] ? fav_trailing[fav_count] : NULL,
        };
        fav_chan[fav_count] = i;
        fav_count++;
    }

    if (fav_count == 0) {
        show_message("Favorites",
            "No favorites saved yet.\nGo to Stations and press \xe2\x98\x85 to add one.");
        return 0;
    }

    int initial = 0;
    for (int j = 0; j < fav_count; j++)
        if (fav_chan[j] == g_playing_idx) { initial = j; break; }

    ap_footer_item footer_base[] = {
        { .button = AP_BTN_B, .label = "Back"                              },
        { .button = AP_BTN_X, .label = "Remove"                            },
        { .button = AP_BTN_A, .label = "Play",   .is_confirm = true        },
    };
    ap_footer_item footer_stop[] = {
        { .button = AP_BTN_B, .label = "Back"                              },
        { .button = AP_BTN_X, .label = "Remove"                            },
        { .button = AP_BTN_Y, .label = "Stop"                              },
        { .button = AP_BTN_A, .label = "Play",   .is_confirm = true        },
    };

    ap_list_opts opts = ap_list_default_opts("Favorites", fav_items, fav_count);
    opts.status_bar    = &g_status_bar;
    opts.action_button = AP_BTN_X;
    opts.initial_index = initial;

    if (g_playing_idx >= 0) {
        opts.footer                  = footer_stop;
        opts.footer_count            = 4;
        opts.secondary_action_button = AP_BTN_Y;
    } else {
        opts.footer       = footer_base;
        opts.footer_count = 3;
    }

    ap_list_result result;
    int rc = ap_list(&opts, &result);

    if (rc == AP_CANCELLED) return 0;

    if (result.action == AP_ACTION_TRIGGERED) {
        favorites_toggle(&g_favorites, g_channels.channels[fav_chan[result.selected_index]].id);
        return 1;
    }
    if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
        player_stop();
        np_stop();
        g_playing_idx = -1;
        return 1;
    }

    int ch_idx = fav_chan[result.selected_index];
    if (ch_idx == g_playing_idx) { screen_now_playing(ch_idx); return 1; }
    if (play_channel(ch_idx)) { load_cover(ch_idx); screen_now_playing(ch_idx); }
    return 1;
}

// ----------------------------------------------------------------
// Main menu  –  folder selector
// Returns 0 to exit app, 1 to loop.
// ----------------------------------------------------------------
// Main menu – custom render loop so MENU can open Settings
// ----------------------------------------------------------------
static void render_main_menu(int cursor) {
    int      sw   = ap_get_screen_width();
    ap_theme *thm = ap_get_theme();
    TTF_Font *fm  = ap_get_font(AP_FONT_LARGE);
    TTF_Font *fsm = ap_get_font(AP_FONT_SMALL);

    /* Count favorites for display */
    int fav_count = 0;
    for (int i = 0; i < g_channels.count; i++)
        if (favorites_contains(&g_favorites, g_channels.channels[i].id))
            fav_count++;

    char stations_val[16], favs_val[16];
    snprintf(stations_val, sizeof(stations_val), "%d", g_channels.count);
    snprintf(favs_val,     sizeof(favs_val),     "%d", fav_count);

    const char *labels[2] = { "Stations",  "Favorites" };
    const char *vals[2]   = { stations_val, favs_val   };

    ap_draw_background();
    ap_draw_status_bar(&g_status_bar);
    ap_draw_screen_title("SomaFM Radio", &g_status_bar);

    if (!fm) { ap_present(); return; }

    int fh     = TTF_FontHeight(fm);
    int item_h = fh + AP_S(14);
    int pad_x  = AP_S(30);
    int start_y = ap_get_status_bar_height() + AP_S(48) + AP_S(12);

    for (int i = 0; i < 2; i++) {
        int  iy  = start_y + i * item_h;
        bool sel = (i == cursor);
        if (sel) ap_draw_rect(0, iy - AP_S(5), sw, item_h, thm->highlight);
        ap_color tc = sel ? thm->highlighted_text : thm->text;
        ap_draw_text(fm, labels[i], pad_x, iy, tc);
        int vw = ap_measure_text(fm, vals[i]);
        ap_draw_text(fm, vals[i], sw - pad_x - vw, iy, tc);
    }

    /* Footer */
    ap_footer_item footer_base[] = {
        { .button = AP_BTN_B,    .label = "Exit"                        },
        { .button = AP_BTN_MENU, .label = "Settings"                    },
        { .button = AP_BTN_A,    .label = "Open", .is_confirm = true    },
    };
    ap_footer_item footer_stop[] = {
        { .button = AP_BTN_B,    .label = "Exit"                        },
        { .button = AP_BTN_MENU, .label = "Settings"                    },
        { .button = AP_BTN_Y,    .label = "Stop"                        },
        { .button = AP_BTN_A,    .label = "Open", .is_confirm = true    },
    };
    if (g_playing_idx >= 0)
        ap_draw_footer(footer_stop, 4);
    else
        ap_draw_footer(footer_base, 3);

    if (fsm) {
        const char *hint = "\xe2\x86\x91\xe2\x86\x93 Select   A: Open   B: Exit   MENU: Settings";
        int hw = ap_measure_text(fsm, hint);
        (void)hw; /* hint already in footer */
    }

    ap_present();
}

static void screen_main_menu(void) {
    int cursor  = 0;
    int scr_off = 0;

    while (1) {
        ap_input_event ev;
        while (ap_poll_input(&ev)) {
            if (!ev.pressed) continue;

            if (ev.button == AP_BTN_SELECT) {
                scr_off = 1;

#ifdef PLATFORM_TG5040
                backlight_off();
#endif
                continue;
            }

            if (scr_off) {
                scr_off = 0;
#ifdef PLATFORM_TG5040
                backlight_on();
#endif
                continue;
            }

            switch (ev.button) {
                case AP_BTN_UP:
                    cursor = cursor > 0 ? cursor - 1 : 1;
                    break;
                case AP_BTN_DOWN:
                    cursor = cursor < 1 ? cursor + 1 : 0;
                    break;
                case AP_BTN_A:
                    if (cursor == 0) while (screen_stations()) ;
                    else             while (screen_favorites()) ;
                    break;
                case AP_BTN_Y:
                    if (g_playing_idx >= 0) {
                        player_stop();
                        np_stop();
                        g_playing_idx = -1;
                    }
                    break;
                case AP_BTN_MENU:
                    screen_settings();
                    break;
                case AP_BTN_B:
                    return;
                default: break;
            }
        }

        if (scr_off) {
            render_screen_off(0);
            SDL_Delay(50);
        } else {
            render_main_menu(cursor);
            SDL_Delay(16);
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
    ap_set_power_handler(false);

    soma_init();
    player_init();
    favorites_load(&g_favorites);
    settings_load(&g_settings);

    if (!wifi_is_active()) {
        show_message("WiFi", "Enabling WiFi, please wait...");
        if (!wifi_enable()) {
            show_message("WiFi Error",
                "Could not connect to WiFi.\nCheck your WiFi settings.");
            goto cleanup;
        }
    }

    if (!soma_fetch_channels(&g_channels) || g_channels.count == 0) {
        show_message("Network Error",
            "Could not load station list.\nCheck your internet connection.");
        goto cleanup;
    }

    screen_main_menu();

cleanup:
    player_cleanup();
    soma_cleanup();
    ap_quit();
    return 0;
}
