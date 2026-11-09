#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double   lat;
    double   lon;
    double   speed_mps;
    double   heading_deg;
    float    accuracy_m;
    bool     valid;
    uint32_t last_update_ms;
} phone_gps_fix_t;

/** What the phone's top banner says, already worded by the phone (single source of truth). */
typedef enum {
    PHONE_GUIDE_STRAIGHT = 0,
    PHONE_GUIDE_TURN,
    PHONE_GUIDE_UTURN,
    PHONE_GUIDE_ARRIVE,
    PHONE_GUIDE_REROUTE,
} phone_guide_kind_t;

typedef struct {
    bool     has_banner;        /**< false: only trip info arrived this time */
    uint8_t  kind;              /**< phone_guide_kind_t */
    int      angle_deg;         /**< turn angle, + = right, - = left */
    char     num[12];           /**< "30", "1.2"; empty when the action stands alone */
    char     unit[16];          /**< "米后", "公里", ... */
    char     action[32];        /**< "右转", "直行", "到达目的地" */
    char     prefix[16];        /**< "进入", "沿"; empty = no second line */
    char     road[64];

    bool     has_info;
    uint32_t remain_m;          /**< metres left to the destination */
    uint32_t remain_s;          /**< seconds left */
    char     eta[8];            /**< "14:35" */
    int      speed_kmh;
} phone_guide_t;

/** One step of the route (the start, or where the road changes), for on-device guidance. */
typedef struct {
    double   lat;
    double   lon;
    int      angle;             /**< turn angle, + = right, - = left; 0 = no turn */
    char     road[40];
} phone_step_t;

typedef struct {
    char     dest_name[64];
    double   dest_lat;
    double   dest_lon;
    double  *lats;
    double  *lons;
    int      point_count;
    double  *tl_lats;
    double  *tl_lons;
    int      tl_count;
    phone_step_t *steps;        /**< NULL when the phone sent none */
    int      step_count;
} phone_route_t;

typedef void (*phone_gps_cb_t)(const phone_gps_fix_t *fix);
typedef void (*phone_route_cb_t)(const phone_route_t *route);
typedef void (*phone_guide_cb_t)(const phone_guide_t *guide);
typedef void (*phone_clear_cb_t)(void);

/**
 * @param route_cb  a route arrives only when the rider presses 开始骑行 on the phone
 * @param guide_cb  banner + trip info while navigating (called with every fix)
 * @param clear_cb  the ride ended: remove the route and go idle
 */
bool phone_gps_start(phone_gps_cb_t gps_cb, phone_route_cb_t route_cb,
                     phone_guide_cb_t guide_cb, phone_clear_cb_t clear_cb);
void phone_gps_get_latest(phone_gps_fix_t *out_fix);

/** @brief true while the phone streams a simulated ride (fixes flagged "sim"). */
bool phone_gps_sim_active(void);

/** @brief Stop the simulated ride from the unit: real GPS counts again and the phone is told to stop on its next fix. */
void phone_gps_sim_stop(void);
bool phone_gps_is_active(void);
/** The app talked to us (a position, a route, a ping...) in the last few seconds - even if it has no GPS fix yet. */
bool phone_gps_recent_contact(void);
bool phone_gps_is_wifi_connected(void);

#ifdef __cplusplus
}
#endif

