/*
 * screen.c — Backlight / screen sleep management
 *
 * How screen on/off actually works on TG5040:
 *   WRONG: DISP_LCD_BACKLIGHT_ENABLE/DISABLE (0x104/0x105) — these ioctls
 *          corrupt the backlight driver state and invert the brightness keys.
 *   RIGHT: Set raw brightness to 0 (off) or the user's saved level (on).
 *          This matches what NextUI's PLAT_enableBacklight() does.
 *
 * Three states (mirrors NextTimer's SCREEN_ON / SCREEN_OFF / SCREEN_HINT):
 *   s_off=false            — screen on, normal operation
 *   s_off=true, s_hint=false  — screen off, backlight raw=0
 *   s_off=true, s_hint=true   — screen off but hint briefly visible;
 *                               backlight restored, turns off after HINT_MS
 */

#include "screen.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>   /* shm_open, mmap */

#define DISP_LCD_SET_BRIGHTNESS  0x102
#define HINT_MS                  2000   /* how long hint backlight stays on */
#define DIM_DURATION_MS          2000   /* gradual dim before full blank */

static uint32_t s_timeout_ms = 30000;   /* configurable via screen_set_timeout() */
static bool     s_off       = false;
static uint32_t s_idle_ms   = 0;
static int      s_saved_lvl = 7;
static bool     s_hint      = false;   /* hint backlight temporarily on */
static uint32_t s_hint_ms   = 0;       /* how long hint has been showing */

/* ── helpers ──────────────────────────────────────────────────────────── */

static int scale_brightness(int lvl) {
    /* Matches scaleBrightness() in NextUI libmsettings for TrimUI Brick. */
    static const int tbl[11] = {1,8,16,32,48,72,96,128,160,192,255};
    if (lvl < 0)  lvl = 0;
    if (lvl > 10) lvl = 10;
    return tbl[lvl];
}

static void set_raw_brightness(int raw) {
    int fd = open("/dev/disp", O_RDWR);
    if (fd >= 0) {
        unsigned long param[4] = {0, (unsigned long)raw, 0, 0};
        ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
        close(fd);
    }
}

typedef struct { int version; int brightness; } MinSettings;

static int read_system_brightness(void) {
    int fd = shm_open("/SharedSettings", O_RDWR, 0644);
    if (fd < 0) return 7;
    MinSettings *s = mmap(NULL, sizeof(MinSettings),
                          PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) return 7;
    int lvl = s->brightness;
    munmap(s, sizeof(MinSettings));
    return (lvl >= 0 && lvl <= 10) ? lvl : 7;
}

static void backlight_on_saved(void) {
    /* NextUI Brick sequence: small kick first, then target. */
    set_raw_brightness(8);
    set_raw_brightness(scale_brightness(s_saved_lvl));
}

/* ── public API ───────────────────────────────────────────────────────── */

void screen_set_timeout(uint32_t ms) {
    s_timeout_ms = ms;   /* 0 = never blank */
}

void screen_init(void) {
    s_off     = false;
    s_idle_ms = 0;
    s_hint    = false;
    s_hint_ms = 0;
}

void screen_activity(void) {
    s_idle_ms = 0;
    if (s_off) screen_on();
}

void screen_update(uint32_t elapsed_ms) {
    if (!s_off) {
        /* Normal: count idle time and auto-blank. */
        s_idle_ms += elapsed_ms;
        if (s_timeout_ms > 0 && s_idle_ms >= s_timeout_ms)
            screen_off();
        return;
    }

    /* Screen is off — advance hint timer if hint is showing. */
    if (s_hint) {
        s_hint_ms += elapsed_ms;
        if (s_hint_ms >= HINT_MS) {
            /* Hint window expired: blank again. */
            s_hint    = false;
            s_hint_ms = 0;
            set_raw_brightness(0);
        }
    }
}

/* Brief backlight flash to show the wake hint to the user.
 * Called when any non-wake key is pressed while screen is off. */
void screen_show_hint(void) {
    if (!s_off) return;
    backlight_on_saved();
    s_hint    = true;
    s_hint_ms = 0;
}

void screen_off(void) {
    if (s_off) return;
    s_saved_lvl = read_system_brightness();
    set_raw_brightness(0);
    s_off     = true;
    s_hint    = false;
    s_hint_ms = 0;
}

void screen_on(void) {
    if (!s_off) return;
    backlight_on_saved();
    s_off     = false;
    s_hint    = false;
    s_hint_ms = 0;
    s_idle_ms = 0;
}

bool screen_is_off(void) {
    return s_off;
}

uint8_t screen_dim_alpha(void) {
    if (s_off || s_timeout_ms == 0 || s_timeout_ms <= DIM_DURATION_MS) return 0;
    uint32_t dim_start = s_timeout_ms - DIM_DURATION_MS;
    if (s_idle_ms < dim_start) return 0;
    uint32_t elapsed = s_idle_ms - dim_start;
    if (elapsed >= DIM_DURATION_MS) return 255;
    return (uint8_t)(255 * elapsed / DIM_DURATION_MS);
}

/* ── Power-button thread ──────────────────────────────────────────────────
 *
 * Apostrophe's built-in power handler suspends the whole device on short
 * press.  We disable that (ap_set_power_handler(false) in main.c) and
 * replace it with our own: short press → screen_off() only, audio keeps
 * playing.  Long press (≥ 1 s) is left to the AXP2202 PMIC hardware
 * shutdown (~5 s hold), so the user can still power off.
 */

static volatile bool s_pwr_running = false;
static pthread_t     s_pwr_thread;

/* Monotonic milliseconds — avoids pulling SDL into screen.c. */
static uint32_t pwr_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static void *pwr_thread_func(void *arg) {
    (void)arg;

    /* Find the /dev/input/eventN that reports KEY_POWER. */
    int fd = -1;
    for (int i = 0; i < 8 && fd < 0; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int tmp = open(path, O_RDONLY | O_NONBLOCK);
        if (tmp < 0) continue;

        /* EVIOCGBIT returns a bitmask of supported key codes. */
        uint8_t bits[KEY_MAX / 8 + 1];
        memset(bits, 0, sizeof(bits));
        if (ioctl(tmp, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
            (bits[KEY_POWER / 8] & (1 << (KEY_POWER % 8)))) {
            fd = tmp;   /* found it */
        } else {
            close(tmp);
        }
    }
    if (fd < 0) return NULL;   /* no power-key device found — give up */

    struct input_event ev;
    while (s_pwr_running) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) {
            usleep(10000);   /* 10 ms poll */
            continue;
        }
        if (ev.type != EV_KEY || ev.code != KEY_POWER || ev.value != 1)
            continue;   /* not a power-button press */

        /* Press detected.  Wait for release or 1-second timeout. */
        uint32_t t0 = pwr_now_ms();
        bool released = false;
        while (s_pwr_running) {
            n = read(fd, &ev, sizeof(ev));
            if (n == sizeof(ev) && ev.type == EV_KEY &&
                ev.code == KEY_POWER && ev.value == 0) {
                released = true;
                break;
            }
            if (pwr_now_ms() - t0 >= 1000) break;
            usleep(10000);
        }

        uint32_t held_ms = pwr_now_ms() - t0;
        if (released && held_ms < 1000) {
            /* Toggle: short press turns screen off or back on. */
            if (screen_is_off())
                screen_on();
            else
                screen_off();
        }
        /* long press: AXP2202 PMIC forces hardware shutdown after ~5 s */
    }

    close(fd);
    return NULL;
}

void screen_init_power(void) {
    s_pwr_running = true;
    pthread_create(&s_pwr_thread, NULL, pwr_thread_func, NULL);
}

void screen_quit_power(void) {
    s_pwr_running = false;
    pthread_join(s_pwr_thread, NULL);
}
