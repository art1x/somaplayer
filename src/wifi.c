#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void save_wifi_setting(int on) {
    const char *shared = getenv("SHARED_USERDATA_PATH");
    if (!shared || !shared[0]) shared = "/mnt/SDCARD/.userdata/shared";

    char path[256];
    snprintf(path, sizeof(path), "%s/minuisettings.txt", shared);

    FILE *f = fopen(path, "r");
    char lines[64][128];
    int count = 0;
    int found = 0;
    if (f) {
        while (count < 63 && fgets(lines[count], sizeof(lines[count]), f))
            count++;
        fclose(f);
    }

    // Update existing wifi= line or append it
    for (int i = 0; i < count; i++) {
        if (strncmp(lines[i], "wifi=", 5) == 0) {
            snprintf(lines[i], sizeof(lines[i]), "wifi=%d\n", on);
            found = 1;
            break;
        }
    }
    if (!found && count < 63) {
        snprintf(lines[count++], sizeof(lines[count]), "wifi=%d\n", on);
    }

    f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < count; i++)
            fputs(lines[i], f);
        fclose(f);
    }
}

static int wpa_state_completed(void) {
    FILE *p = popen(
        "wpa_cli -p /etc/wifi/sockets -i wlan0 status 2>/dev/null"
        " | grep '^wpa_state=' | cut -d= -f2",
        "r");
    if (!p) return 0;
    char state[32] = {0};
    fgets(state, sizeof(state), p);
    pclose(p);
    state[strcspn(state, "\n")] = '\0';
    return strcmp(state, "COMPLETED") == 0;
}

int wifi_is_active(void) {
#ifdef PLATFORM_TG5040
    return wpa_state_completed();
#else
    return 1;
#endif
}

int wifi_enable(void) {
#ifdef PLATFORM_TG5040
    if (wifi_is_active()) return 1;

    // NextUI script starts rfkill unblock, wpa_supplicant and udhcpc
    const char *platform = getenv("PLATFORM");
    if (!platform || !platform[0]) platform = "tg5040";

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "/mnt/SDCARD/.system/%s/etc/wifi/wifi_init.sh start > /dev/null 2>&1",
        platform);
    system(cmd);
    usleep(1000000);  // give wpa_supplicant 1s to start

    system("wpa_cli -p /etc/wifi/sockets -i wlan0 enable_network all > /dev/null 2>&1");
    system("wpa_cli -p /etc/wifi/sockets -i wlan0 reconnect > /dev/null 2>&1");

    // Wait up to 10s (20 x 500ms)
    for (int i = 0; i < 20; i++) {
        usleep(500000);
        if (wpa_state_completed()) {
            save_wifi_setting(1);
            return 1;
        }
    }
    return 0;
#else
    return 1;
#endif
}
