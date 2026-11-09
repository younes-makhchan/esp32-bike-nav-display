#pragma once

#include "lvgl.h"

/**
 * The pet: two big eyes that look alive (blink, look around, happy, surprised, sleepy, wink...).
 * It owns one LVGL screen; the launcher decides when to show it.
 * Call all of these with the LVGL lock held.
 */

/** @brief Build the pet screen. Returns it so the launcher can load it and listen for touches. */
lv_obj_t *pet_init(void);

/** @brief Start animating (eyes open from closed, like waking up). Call right before loading the screen. */
void pet_start(void);

/** @brief Stop animating (screen left or switched off): no CPU is used while stopped. */
void pet_stop(void);

/** @brief Play the "falling asleep" animation: the eyes slowly close and stay closed. */
void pet_sleep(void);
