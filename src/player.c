#include "player.h"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>

static pid_t g_pid   = -1;
static int   g_error = 0;  /* set when mpg123 exec fails (exit 127) */

void player_init(void) {}

void player_cleanup(void) {
    player_stop();
}

int player_play(const char *url) {
    player_stop();
    g_error = 0;

    g_pid = fork();
    if (g_pid < 0) {
        g_error = 1;
        return 0;
    }
    if (g_pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        /* -q: quiet, --no-terminal: no terminal interaction,
           the URL is a direct ICY/HTTP MP3 stream */
        execlp("mpg123", "mpg123", "-q", "--no-terminal", url, NULL);
        _exit(127);  /* exec failed */
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
    if (g_error)   return PLAYER_ERROR;
    if (g_pid <= 0) return PLAYER_STOPPED;

    int status;
    pid_t r = waitpid(g_pid, &status, WNOHANG);
    if (r == 0) return PLAYER_PLAYING;

    g_pid = -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        g_error = 1;
        return PLAYER_ERROR;
    }
    return PLAYER_STOPPED;
}
