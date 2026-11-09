#pragma once

#include "lvgl.h"

/**
 * The "rider" screen: a pixel-art self-portrait next to a yellow mountain bike (wheels turn).
 * It owns one LVGL screen; the launcher decides when to show it and draws the back button on it.
 * Call all of these with the LVGL lock held.
 */

/** @brief Build the screen. Returns it so the launcher can load it. */
lv_obj_t *rider_init(void);

/** @brief Start the wheel animation. Call right before loading the screen. */
void rider_start(void);

/** @brief Stop the animation (screen left): no CPU is used while stopped. */
void rider_stop(void);
