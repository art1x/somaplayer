#pragma once
/*
 * screen.h — Backlight / screen sleep management
 *
 * The screen turns off automatically after 30 seconds of inactivity during
 * playback (audio continues), or immediately when the user presses Select.
 *
 * Screen off:  set raw brightness to 0 via DISP_LCD_SET_BRIGHTNESS (0x102)
 * Screen on:   restore saved brightness level from NextUI shared settings
 *
 * Do NOT use DISP_LCD_BACKLIGHT_ENABLE/DISABLE (0x104/0x105) — those ioctls
 * corrupt the backlight driver state and invert the hardware brightness keys.
 *
 * Wake gesture: Select + A
 * While screen is off, any other key shows a hint message.
 */

#include <stdbool.h>
#include <stdint.h>

/* Set the idle timeout in milliseconds (default 30 000).
   Call before screen_init().  A value of 0 disables auto-blank. */
void screen_set_timeout(uint32_t ms);

/* Initialise screen state.  Call once at startup. */
void screen_init(void);

/* Notify the screen module that user input occurred.  Resets the idle timer. */
void screen_activity(void);

/* Call once per frame.  Turns the screen off if the idle timer has expired.
 * elapsed_ms = milliseconds since the last frame (for the timer). */
void screen_update(uint32_t elapsed_ms);

/* Turn the screen off immediately (e.g. user pressed Select). */
void screen_off(void);

/* Briefly turn the backlight on to show the wake hint, then blank again.
 * Call when a non-wake key is pressed while the screen is off. */
void screen_show_hint(void);

/* Turn the screen back on (e.g. Select + A pressed). */
void screen_on(void);

/* Returns true if the screen is currently off. */
bool screen_is_off(void);

/* Returns 0–255: how much to dim the rendered frame before the screen goes off.
   0 = normal brightness, 255 = fully dimmed (screen about to blank).
   The caller draws a black overlay with this alpha to simulate gradual dimming. */
uint8_t screen_dim_alpha(void);

/* Start power-button thread: short press → screen_off(), audio continues.
 * Call after screen_init().  Disable Apostrophe's handler first with
 * ap_set_power_handler(false) so it doesn't race and trigger suspend. */
void screen_init_power(void);

/* Stop the power-button thread.  Call before ap_quit(). */
void screen_quit_power(void);
