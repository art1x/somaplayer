// ================================================================
// main.c  –  SomaFM Radio Player for TrimUI Brick / NextUI
//
// Controls (Station List):
//   ↑ / ↓      → Navigate stations
//   A           → Play selected station
//   Y           → Stop playback
//   B / MENU    → Exit
//
// Controls (Now Playing):
//   A           → Stop
//   B           → Back to list (music keeps playing)
// ================================================================

#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_WIDGETS_IMPLEMENTATION
#include "apostrophe_widgets.h"

#include "soma.h"
#include "player.h"
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SomaChannelList g_channels   = {0};
static int             g_playing_idx = -1;  /* -1 = stopped */

static ap_status_bar_opts g_status_bar = {
    .show_clock   = AP_CLOCK_AUTO,
    .show_battery = true,
    .show_wifi    = true,
};

// ----------------------------------------------------------------
// Helper: simple info message (OK to dismiss)
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
// Now Playing screen
// ----------------------------------------------------------------
static void screen_now_playing(int idx) {
    SomaChannel *ch = &g_channels.channels[idx];

    char listeners_buf[32];
    snprintf(listeners_buf, sizeof(listeners_buf), "%d", ch->listeners);

    ap_detail_info_pair info[] = {
        { .key = "Genre",     .value = ch->genre[0]  ? ch->genre      : "—" },
        { .key = "Listeners", .value = ch->listeners  ? listeners_buf  : "—" },
        { .key = "Stream",    .value = ch->stream_url[0] ? ch->stream_url : "—" },
    };

    ap_detail_section sections[2];
    memset(sections, 0, sizeof(sections));
    int section_count = 0;

    if (ch->description[0]) {
        sections[section_count].type        = AP_SECTION_DESCRIPTION;
        sections[section_count].title       = NULL;
        sections[section_count].description = ch->description;
        section_count++;
    }

    sections[section_count].type       = AP_SECTION_INFO;
    sections[section_count].title      = NULL;
    sections[section_count].info_pairs = info;
    sections[section_count].info_count = 3;
    section_count++;

    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back"                             },
        { .button = AP_BTN_A, .label = "Stop", .is_confirm = true         },
    };

    /* Show playback status in title */
    char title_buf[80];
    PlayerStatus st = player_status();
    const char *status_icon = (st == PLAYER_PLAYING) ? "● " :
                              (st == PLAYER_ERROR)   ? "✕ " : "■ ";
    snprintf(title_buf, sizeof(title_buf), "%s%s", status_icon, ch->title);

    ap_detail_opts opts = {
        .title         = title_buf,
        .sections      = sections,
        .section_count = section_count,
        .footer        = footer,
        .footer_count  = 2,
        .status_bar    = &g_status_bar,
    };

    ap_detail_result result;
    ap_detail_screen(&opts, &result);

    if (result.action == AP_DETAIL_ACTION) {
        /* A = Stop */
        player_stop();
        g_playing_idx = -1;
    }
    /* B = back, playback continues */
}

// ----------------------------------------------------------------
// Station List  –  main screen
// Returns 0 when user exits, 1 to keep running.
// ----------------------------------------------------------------
static int screen_station_list(void) {
    if (g_channels.count == 0) return 0;

    /* Build list items */
    static ap_list_item  items[SOMA_MAX_CHANNELS];
    static char          trailing[SOMA_MAX_CHANNELS][48];

    for (int i = 0; i < g_channels.count; i++) {
        SomaChannel *ch = &g_channels.channels[i];
        const char  *play_marker = (i == g_playing_idx) ? "▶ " : "";

        if (ch->genre[0])
            snprintf(trailing[i], sizeof(trailing[i]), "%s%s", play_marker, ch->genre);
        else
            snprintf(trailing[i], sizeof(trailing[i]), "%s", play_marker);

        items[i] = (ap_list_item){
            .label        = ch->title,
            .trailing_text = trailing[i][0] ? trailing[i] : NULL,
        };
    }

    /* Footer: Stop only shown when something is playing */
    ap_footer_item footer_play[] = {
        { .button = AP_BTN_B, .label = "Exit"                          },
        { .button = AP_BTN_A, .label = "Play", .is_confirm = true      },
    };
    ap_footer_item footer_stop[] = {
        { .button = AP_BTN_B, .label = "Exit"                          },
        { .button = AP_BTN_Y, .label = "Stop"                          },
        { .button = AP_BTN_A, .label = "Play", .is_confirm = true      },
    };

    ap_list_opts opts = ap_list_default_opts("SomaFM Radio",
                                             items, g_channels.count);
    opts.status_bar   = &g_status_bar;
    if (g_playing_idx >= 0) {
        opts.footer                  = footer_stop;
        opts.footer_count            = 3;
        opts.secondary_action_button = AP_BTN_Y;
    } else {
        opts.footer       = footer_play;
        opts.footer_count = 2;
    }
    opts.initial_index = (g_playing_idx >= 0) ? g_playing_idx : 0;

    ap_list_result result;
    int rc = ap_list(&opts, &result);

    if (rc == AP_CANCELLED) {
        /* B or MENU pressed → exit */
        player_stop();
        g_playing_idx = -1;
        return 0;
    }

    /* Y = Stop action */
    if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
        player_stop();
        g_playing_idx = -1;
        return 1;
    }

    int idx = result.selected_index;

    /* If the user selects the currently playing station → show now-playing */
    if (idx == g_playing_idx) {
        screen_now_playing(idx);
        return 1;
    }

    /* Start a new station */
    player_stop();
    if (!player_play(g_channels.channels[idx].stream_url)) {
        show_message("Error", "Could not start playback.\nIs mpg123 available?");
        return 1;
    }

    PlayerStatus st = player_status();
    if (st == PLAYER_ERROR) {
        show_message("Error", "mpg123 not found.\nPlease install it on the device.");
        g_playing_idx = -1;
        return 1;
    }

    g_playing_idx = idx;
    screen_now_playing(idx);
    return 1;
}

// ----------------------------------------------------------------
// main
// ----------------------------------------------------------------
int main(void) {
    if (ap_init() != AP_OK) return 1;

    soma_init();
    player_init();

    /* WiFi check */
    if (!wifi_is_active()) {
        show_message("WiFi", "Enabling WiFi, please wait...");
        if (!wifi_enable()) {
            show_message("WiFi Error",
                "Could not connect to WiFi.\nCheck your WiFi settings.");
            goto cleanup;
        }
    }

    /* Fetch channel list (blocks up to 15 s) */
    if (!soma_fetch_channels(&g_channels) || g_channels.count == 0) {
        show_message("Network Error",
            "Could not load station list.\nCheck your internet connection.");
        goto cleanup;
    }

    /* Main loop */
    while (screen_station_list())
        ;

cleanup:
    player_cleanup();
    soma_cleanup();
    ap_cleanup();
    return 0;
}
