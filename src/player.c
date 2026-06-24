#include "player.h"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>  /* shm_open, mmap — audio sink hotplug detection */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

static pid_t g_pid   = -1;
static int   g_error = 0;
static char  g_url[2048] = ""; /* remembered so a sink change can rejoin the stream */

/* ── audio sink hotplug (USB-C audio interface / Bluetooth speaker) ─────────
 * NextUI's "audiomon" daemon rewrites ~/.asoundrc (ALSA's "default" device)
 * and a field in a shared-memory block ("/SharedSettings") whenever a USB
 * audio interface or Bluetooth speaker is plugged or unplugged.  ALSA only
 * re-reads .asoundrc when a process opens a *fresh* PCM handle, so a
 * long-running mpg123 keeps talking to whatever device was "default" when it
 * started.  We poll the shared "audiosink" field once a frame and, on a
 * change, kill + restart mpg123 on the same stream URL — there's no seek
 * position to preserve for live radio, so this is just a clean rejoin. */

/* Mirrors the tail of NextUI's `SettingsV10` struct (libmsettings, tg5040).
 * We only need `audiosink`; `version` lets us bail out instead of reading
 * garbage if a future NextUI build reorders the struct. */
typedef struct {
    int version;    /* offset   0 — must equal NEXTUI_SETTINGS_VERSION */
    int _skip[26];  /* brightness, colortemperature, ... unused[1] */
    int jack;       /* offset 108 — headphone jack inserted (unused here) */
    int audiosink;  /* offset 112 — 0 default/speaker, 1 bluetooth, 2 USB DAC */
} nextui_settings;

#define NEXTUI_SETTINGS_VERSION 10

/* Returns the current AUDIO_SINK_* value, or -1 if the shared-memory block
 * isn't present (e.g. the Linux dev build) or its version doesn't match —
 * either way we'd rather do nothing than act on garbage. */
static int read_audio_sink(void) {
    int fd = shm_open("/SharedSettings", O_RDWR, 0644);
    if (fd < 0) return -1;
    nextui_settings *s = mmap(NULL, sizeof(nextui_settings), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) return -1;
    int sink = (s->version == NEXTUI_SETTINGS_VERSION) ? s->audiosink : -1;
    munmap(s, sizeof(nextui_settings));
    return sink;
}

static int g_last_audio_sink = -1; /* -1 = not read yet (don't react to it) */

void player_init(void) {}

void player_cleanup(void) {
    player_stop();
}

int player_play(const char *url) {
    player_stop();
    g_error = 0;

    /* Remember the URL (even on a later fork failure) so a sink change can
     * rejoin the same stream — strncpy is safe here even when called with
     * url == g_url (player_check_audio_sink() restarting in place). */
    if (url != g_url) {
        strncpy(g_url, url, sizeof(g_url) - 1);
        g_url[sizeof(g_url) - 1] = '\0';
    }

    g_pid = fork();
    if (g_pid < 0) { g_error = 1; return 0; }

    if (g_pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        if (access("./mpg123", X_OK) == 0)
            execl("./mpg123", "mpg123", "-q", url, NULL);
        else
            execlp("mpg123", "mpg123", "-q", url, NULL);
        _exit(127);
    }
    return 1;
}

void player_stop(void) {
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        waitpid(g_pid, NULL, 0);
        g_pid = -1;
    }
    g_error = 0;
}

PlayerStatus player_status(void) {
    if (g_error)    return PLAYER_ERROR;
    if (g_pid <= 0) return PLAYER_STOPPED;

    int status;
    pid_t r = waitpid(g_pid, &status, WNOHANG);
    if (r == 0) return PLAYER_PLAYING;

    g_pid = -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        g_error = 1;
        return PLAYER_ERROR;
    }
    return PLAYER_STOPPED;
}

void player_check_audio_sink(void) {
    int sink = read_audio_sink();
    if (sink < 0) return; /* shm missing or unexpected layout — ignore */

    /* g_pid > 0 alone isn't enough — a just-exited stream leaves a stale pid
     * until player_status() reaps it, so re-check liveness with WNOHANG. */
    if (g_last_audio_sink >= 0 && sink != g_last_audio_sink &&
        g_pid > 0 && waitpid(g_pid, NULL, WNOHANG) == 0) {
        player_play(g_url); /* kills the old mpg123, rejoins the same stream */
    }

    g_last_audio_sink = sink;
}

