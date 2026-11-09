#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "phone_gps.h"

/**
 * Turn-by-turn banner computed ON THE DEVICE from the route's steps and how far along
 * it the rider is. It mirrors guideForWatch() in amap_companion.html (same decisions,
 * same wording), but needs no help from the phone page - which is frozen when the phone
 * screen is off. Keep the two in sync.
 *
 * Pure logic (no LVGL / Arduino), so it can be tested on a computer.
 */

#define NAV_GUIDE_MAX_STEPS 96

typedef struct {
    uint32_t dist_m;        /**< metres from the route start to this step */
    int16_t  angle;         /**< turn angle, + = right, - = left; 0 = no turn (road name change only) */
    char     road[40];      /**< road entered at this step */
} nav_step_t;

/** @brief Replace the steps. @p total_m is the route length in the same metric as dist_m. */
void nav_guide_set_steps(const nav_step_t *steps, int count, uint32_t total_m);
void nav_guide_clear(void);
bool nav_guide_has_steps(void);

/** @brief Banner for a rider @p traveled_m metres along the route. false if there are no steps. */
bool nav_guide_compute(uint32_t traveled_m, phone_guide_t *out);
