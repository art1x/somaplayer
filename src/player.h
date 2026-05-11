#ifndef PLAYER_H
#define PLAYER_H

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_ERROR,   /* mpg123 not found or exec failed */
} PlayerStatus;

void         player_init(void);
void         player_cleanup(void);
int          player_play(const char *stream_url);  /* 1 = started, 0 = fork error */
void         player_stop(void);
PlayerStatus player_status(void);

#endif
