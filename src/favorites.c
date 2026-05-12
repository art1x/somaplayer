#include "favorites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Returns "$SHARED_USERDATA_PATH/somaplayer" and ensures the directory exists. */
static const char *userdata_dir(void) {
    static char dir[256];
    if (dir[0]) return dir;
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (!base || !base[0]) base = "/mnt/SDCARD/.userdata/shared";
    snprintf(dir, sizeof(dir), "%s/somaplayer", base);
    mkdir(dir, 0755);
    return dir;
}

static void fav_path(char *buf, size_t size) {
    snprintf(buf, size, "%s/favorites.txt", userdata_dir());
}

void favorites_load(FavoriteList *f) {
    f->count = 0;
    char path[280];
    fav_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    while (f->count < SOMA_MAX_CHANNELS &&
           fgets(f->ids[f->count], SOMA_ID_LEN, fp)) {
        int len = (int)strlen(f->ids[f->count]);
        while (len > 0 &&
               (f->ids[f->count][len-1] == '\n' || f->ids[f->count][len-1] == '\r'))
            f->ids[f->count][--len] = '\0';
        if (len > 0)
            f->count++;
    }
    fclose(fp);
}

void favorites_save(const FavoriteList *f) {
    char path[280];
    fav_path(path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 0; i < f->count; i++)
        fprintf(fp, "%s\n", f->ids[i]);
    fclose(fp);
}

int favorites_contains(const FavoriteList *f, const char *id) {
    if (!id || !id[0]) return 0;
    for (int i = 0; i < f->count; i++)
        if (strcmp(f->ids[i], id) == 0) return 1;
    return 0;
}

void favorites_toggle(FavoriteList *f, const char *id) {
    if (!id || !id[0]) return;
    for (int i = 0; i < f->count; i++) {
        if (strcmp(f->ids[i], id) == 0) {
            for (int j = i; j < f->count - 1; j++)
                memcpy(f->ids[j], f->ids[j + 1], SOMA_ID_LEN);
            f->count--;
            favorites_save(f);
            return;
        }
    }
    if (f->count < SOMA_MAX_CHANNELS) {
        strncpy(f->ids[f->count], id, SOMA_ID_LEN - 1);
        f->ids[f->count][SOMA_ID_LEN - 1] = '\0';
        f->count++;
        favorites_save(f);
    }
}
