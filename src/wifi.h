#ifndef WIFI_H
#define WIFI_H

// Returns 1 if WiFi is connected, 0 if off/unavailable
int wifi_is_active(void);

// Attempts to enable WiFi. Returns 1 if successful, 0 if failed.
int wifi_enable(void);

#endif
