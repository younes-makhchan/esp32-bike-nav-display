#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include "phone_gps.h"

#ifdef __cplusplus
extern "C" {
#endif

bool map_manager_init(lv_obj_t* parent_screen);
/** A position fix (AMap coordinates). With route steps loaded this also refreshes the banner on the device. */
void map_manager_update_gps(double lat, double lon, double heading_deg, double speed_mps);
void map_manager_set_route(const double *lats, const double *lons, int point_count, const char *dest_name);
void map_manager_set_traffic_lights(const double *lats, const double *lons, int count);
void map_manager_clear_route(void);
/** Steps of the route just set (call right after map_manager_set_route): enables on-device guidance. */
void map_manager_set_steps(const phone_step_t *steps, int count);
void map_manager_center_on_gps(void);
void map_manager_set_status_text(const char* text);
/** Mirror the phone: banner (icon, distance, action, road) and trip info (remaining, ETA, speed). */
void map_manager_update_nav(const phone_guide_t *guide);
void map_manager_zoom_in(void);
void map_manager_zoom_out(void);
int map_manager_get_zoom(void);
/** Show or hide the map's overlay widgets (card, zoom, buttons) - they live on the shared top layer. */
void map_manager_set_visible(bool visible);

#ifdef __cplusplus
}
#endif

