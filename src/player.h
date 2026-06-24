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

/* Call once a frame while a stream may be playing.  NextUI's audiomon daemon
 * updates the system audio sink (speaker / USB-C interface / Bluetooth)
 * asynchronously when a device is plugged or unplugged; this notices the
 * change and rejoins the current stream so mpg123 opens a fresh ALSA handle
 * against the device that's actually active now.  No-op if nothing is
 * playing or the platform doesn't expose the sink. */
void         player_check_audio_sink(void);


#endif
