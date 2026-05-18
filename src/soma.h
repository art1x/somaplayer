#ifndef SOMA_H
#define SOMA_H

#define SOMA_MAX_CHANNELS 80
#define SOMA_ID_LEN       32
#define SOMA_TITLE_LEN    64
#define SOMA_DESC_LEN     280
#define SOMA_GENRE_LEN    64
#define SOMA_URL_LEN      256
#define SOMA_NP_LEN       128

typedef struct {
    char id[SOMA_ID_LEN];
    char title[SOMA_TITLE_LEN];
    char description[SOMA_DESC_LEN];
    char genre[SOMA_GENRE_LEN];
    char stream_url[SOMA_URL_LEN];   /* direct HTTP MP3 stream */
    char image_url[SOMA_URL_LEN];    /* 256 px cover art (largeimage) */
    int  listeners;
} SomaChannel;

typedef struct {
    SomaChannel channels[SOMA_MAX_CHANNELS];
    int count;
} SomaChannelList;

void soma_init(void);
void soma_cleanup(void);

/* Fetch and parse the SomaFM channel list.
   Returns 1 on success (out->count > 0), 0 on network or parse error. */
int soma_fetch_channels(SomaChannelList *out);

/* Load channel list from a local JSON cache file written by a previous run.
   Returns 1 on success (out->count > 0), 0 if the file is missing or corrupt. */
int soma_channels_cache_load(const char *path, SomaChannelList *out);

/* Fetch fresh channels from the SomaFM API and overwrite the cache file.
   Safe to call from a background thread — does NOT touch g_channels.
   Returns 1 on success (file written), 0 on network or parse error. */
int soma_channels_cache_refresh(const char *path);

/* Fetch now-playing track for a channel.
   title and artist buffers must each be SOMA_NP_LEN bytes.
   Returns 1 on success, 0 on failure. */
int soma_fetch_now_playing(const char *channel_id,
                           char *title, char *artist);

#endif
