#ifndef SOMA_H
#define SOMA_H

#define SOMA_MAX_CHANNELS 80
#define SOMA_ID_LEN       32
#define SOMA_TITLE_LEN    64
#define SOMA_DESC_LEN     280
#define SOMA_GENRE_LEN    64
#define SOMA_URL_LEN      256

typedef struct {
    char id[SOMA_ID_LEN];
    char title[SOMA_TITLE_LEN];
    char description[SOMA_DESC_LEN];
    char genre[SOMA_GENRE_LEN];
    char stream_url[SOMA_URL_LEN];  /* direct HTTP MP3 stream */
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

#endif
