#pragma once

#include <Arduino.h>
#include "Ryzobee.h"
#include "lvgl.h"

extern Ryzobee board;

/**
 * @brief Initialize LVGL and bridge display and touch with RootMaker LovyanGFX
 */
void rootmaker_display_init(void);

/**
 * @brief Thread-safe lock for LVGL operations
 */
bool rootmaker_display_lock(uint32_t timeout_ms = 1000);

/**
 * @brief Thread-safe unlock for LVGL operations
 */
void rootmaker_display_unlock(void);

