// ================================================================
// soma.c  –  SomaFM API client
//
// Fetches https://api.somafm.com/channels.json and parses it into
// a SomaChannelList.  Stream URL priority:
//   1. streams[].url where format == "mp3"  (direct ICY stream)
//   2. Constructed: https://ice1.somafm.com/<id>-128-mp3
//   3. plsfile URL (mpg123 can parse PLS over HTTP)
// ================================================================

#include "soma.h"

#include <curl/curl.h>
#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOMA_API_URL \
    "https://api.somafm.com/channels.json"

typedef struct {
    char  *data;
    size_t size;
} CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, CurlBuf *buf) {
    size_t bytes = size * nmemb;
    char *tmp = realloc(buf->data, buf->size + bytes + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, bytes);
    buf->size += bytes;
    buf->data[buf->size] = '\0';
    return bytes;
}

static char *http_get(const char *url, long timeout_sec) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuf buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "SomaPak/1.0 (NextUI TrimUI; github.com/art1x)");
#ifdef PLATFORM_TG5040
    curl_easy_setopt(curl, CURLOPT_CAINFO, "./cacert.pem");
#endif

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

static int channel_cmp(const void *a, const void *b) {
    return strcmp(((const SomaChannel *)a)->title,
                  ((const SomaChannel *)b)->title);
}

/* Copy at most (dst_size-1) chars, ensure NUL-termination. */
static void safe_copy(char *dst, const char *src, size_t dst_size) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void soma_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void soma_cleanup(void) {
    curl_global_cleanup();
}

int soma_fetch_channels(SomaChannelList *out) {
    out->count = 0;

    char *json = http_get(SOMA_API_URL, 15L);
    if (!json) return 0;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;

    cJSON *arr = cJSON_GetObjectItem(root, "channels");
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return 0;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > SOMA_MAX_CHANNELS) n = SOMA_MAX_CHANNELS;

    int count = 0;
    for (int i = 0; i < n && count < SOMA_MAX_CHANNELS; i++) {
        cJSON *ch = cJSON_GetArrayItem(arr, i);
        if (!ch) continue;

        SomaChannel *c = &out->channels[count];
        memset(c, 0, sizeof(*c));

        cJSON *jtitle  = cJSON_GetObjectItem(ch, "title");
        cJSON *jdesc   = cJSON_GetObjectItem(ch, "description");
        cJSON *jgenre  = cJSON_GetObjectItem(ch, "genre");
        cJSON *jid     = cJSON_GetObjectItem(ch, "id");
        cJSON *jlist   = cJSON_GetObjectItem(ch, "listeners");
        cJSON *jimg    = cJSON_GetObjectItem(ch, "image");

        if (!jtitle || !cJSON_IsString(jtitle)) continue;

        safe_copy(c->title,       jtitle->valuestring,  SOMA_TITLE_LEN);
        if (jdesc  && cJSON_IsString(jdesc))
            safe_copy(c->description, jdesc->valuestring,   SOMA_DESC_LEN);
        if (jgenre && cJSON_IsString(jgenre))
            safe_copy(c->genre,       jgenre->valuestring,  SOMA_GENRE_LEN);
        if (jid    && cJSON_IsString(jid))
            safe_copy(c->id,          jid->valuestring,     SOMA_ID_LEN);
        if (jlist  && cJSON_IsNumber(jlist))
            c->listeners = (int)jlist->valuedouble;
        if (jimg   && cJSON_IsString(jimg))
            safe_copy(c->image_url, jimg->valuestring, SOMA_URL_LEN);

        /* 1. Try streams[] array – prefer http:// mp3 first, https:// mp3 as fallback */
        cJSON *jstreams = cJSON_GetObjectItem(ch, "streams");
        if (jstreams && cJSON_IsArray(jstreams)) {
            int sn = cJSON_GetArraySize(jstreams);
            char https_fallback[SOMA_URL_LEN] = {0};
            for (int j = 0; j < sn; j++) {
                cJSON *s    = cJSON_GetArrayItem(jstreams, j);
                cJSON *sfmt = cJSON_GetObjectItem(s, "format");
                cJSON *surl = cJSON_GetObjectItem(s, "url");
                if (!sfmt || !surl ||
                    !cJSON_IsString(sfmt) || !cJSON_IsString(surl) ||
                    strcmp(sfmt->valuestring, "mp3") != 0) continue;
                if (strncmp(surl->valuestring, "http://", 7) == 0) {
                    safe_copy(c->stream_url, surl->valuestring, SOMA_URL_LEN);
                    break;
                }
                if (!https_fallback[0])
                    safe_copy(https_fallback, surl->valuestring, SOMA_URL_LEN);
            }
            if (!c->stream_url[0] && https_fallback[0])
                safe_copy(c->stream_url, https_fallback, SOMA_URL_LEN);
        }

        /* 2. Construct from id if streams[] gave nothing */
        if (c->stream_url[0] == '\0' && c->id[0] != '\0') {
            char id_copy[SOMA_ID_LEN];
            memcpy(id_copy, c->id, SOMA_ID_LEN);
            snprintf(c->stream_url, SOMA_URL_LEN,
                "http://ice1.somafm.com/%s-128-mp3", id_copy);
        }

        /* 3. Fall back to plsfile (mpg123 can parse PLS over HTTP) */
        if (c->stream_url[0] == '\0') {
            cJSON *jpls = cJSON_GetObjectItem(ch, "plsfile");
            if (jpls && cJSON_IsString(jpls))
                safe_copy(c->stream_url, jpls->valuestring, SOMA_URL_LEN);
        }

        count++;
    }

    cJSON_Delete(root);
    out->count = count;

    qsort(out->channels, (size_t)out->count, sizeof(SomaChannel), channel_cmp);
    return count > 0 ? 1 : 0;
}

int soma_fetch_now_playing(const char *channel_id,
                           char *title, char *artist) {
    title[0] = artist[0] = '\0';

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.somafm.com/songs/%s.json", channel_id);

    char *json = http_get(url, 5L);
    if (!json) return 0;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;

    int ok = 0;
    cJSON *songs = cJSON_GetObjectItem(root, "songs");
    if (songs && cJSON_IsArray(songs) && cJSON_GetArraySize(songs) > 0) {
        cJSON *first   = cJSON_GetArrayItem(songs, 0);
        cJSON *jtitle  = cJSON_GetObjectItem(first, "title");
        cJSON *jartist = cJSON_GetObjectItem(first, "artist");
        if (jtitle  && cJSON_IsString(jtitle))
            safe_copy(title,  jtitle->valuestring,  SOMA_NP_LEN);
        if (jartist && cJSON_IsString(jartist))
            safe_copy(artist, jartist->valuestring, SOMA_NP_LEN);
        ok = 1;
    }

    cJSON_Delete(root);
    return ok;
}
