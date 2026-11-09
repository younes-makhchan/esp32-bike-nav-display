#pragma once

#include "lvgl.h"

/**
 * Home screen with app icons, plus the "scan to connect" screen.
 *
 * Flow for the navigation app: tap its icon -> QR code of the device's web page ->
 * the phone scans it and opens the page -> its first GPS fix arrives -> the map
 * opens by itself. If the phone is already connected the QR step is skipped.
 *
 * Adding an app = one line in `s_apps[]` in launcher.cpp.
 * Call with the LVGL lock held.
 *
 * @param map_screen the screen that carries the map (see map_manager_init)
 */
void launcher_init(lv_obj_t *map_screen);

/** @brief true while the map is on screen (the physical button controls zoom only then). */
bool launcher_map_active(void);

/** @brief Wake the pet screen (physical button): goes to the home screen. Call with the LVGL lock held. */
void launcher_wake(void);

/** @brief true while the pet (or the blacked-out screen) is shown. */
bool launcher_pet_active(void);
