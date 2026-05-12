#ifndef PLAYER_H
#define PLAYER_H

#include <sys/types.h>

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_ERROR,
} PlayerStatus;

void         player_init(void);
void         player_cleanup(void);
int          player_play(const char *stream_url);
void         player_stop(void);
PlayerStatus player_status(void);

/* Background-playback support */
pid_t        player_get_pid(void);   /* -1 if not playing */
void         player_detach(void);    /* disown mpg123 without killing */
void         player_adopt(pid_t pid);/* take ownership of a running mpg123 */

#endif
