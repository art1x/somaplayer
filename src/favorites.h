#ifndef FAVORITES_H
#define FAVORITES_H

#include "soma.h"

typedef struct {
    char ids[SOMA_MAX_CHANNELS][SOMA_ID_LEN];
    int  count;
} FavoriteList;

void favorites_load(FavoriteList *f);
void favorites_save(const FavoriteList *f);
int  favorites_contains(const FavoriteList *f, const char *id);
void favorites_toggle(FavoriteList *f, const char *id);

#endif
