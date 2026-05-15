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


#endif
