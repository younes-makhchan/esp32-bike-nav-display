#pragma once

#include <stdint.h>

/**
 * Screen brightness: 1..100 percent, remembered across restarts (flash, via Preferences).
 * Never lets the screen go fully dark, so it can always be turned back up.
 */
#define BRIGHTNESS_MIN_PCT 10

/** @brief Saved brightness in percent (100 if never set). */
int brightness_load(void);

/** @brief Set the backlight now. @p save also remembers it. */
void brightness_set(int pct, bool save);

/** @brief Backlight fully off (screen "blacked out"), without touching the saved level. */
void brightness_off(void);
