#include <Arduino.h>
#include "Ryzobee.h"
#include "rootmaker_display.h"
#include "map_manager.h"
#include "phone_gps.h"
#include "launcher.h"
#include "brightness.h"

Ryzobee board(RYZOBEE_ROOTMAKER);

static void on_phone_gps_fix(const phone_gps_fix_t *fix)
{
    if (fix && fix->valid) {
        if (rootmaker_display_lock(100)) {
            map_manager_update_gps(fix->lat, fix->lon, fix->heading_deg, fix->speed_mps);
            rootmaker_display_unlock();
        }
    }
}

static void on_phone_guide(const phone_guide_t *guide)
{
    if (guide && rootmaker_display_lock(100)) {
        map_manager_update_nav(guide);
        rootmaker_display_unlock();
    }
}

static void on_phone_clear(void)
{
    if (rootmaker_display_lock(1000)) {
        map_manager_clear_route();
        rootmaker_display_unlock();
    }
}

static void on_phone_route(const phone_route_t *route)
{
    if (route && route->point_count >= 2) {
        if (rootmaker_display_lock(1000)) {
            map_manager_set_route(route->lats, route->lons, route->point_count, route->dest_name);
            map_manager_set_steps(route->steps, route->step_count);
            if (route->tl_lats && route->tl_lons && route->tl_count > 0) {
                map_manager_set_traffic_lights(route->tl_lats, route->tl_lons, route->tl_count);
            }
            rootmaker_display_unlock();
        }
    }
}

void setup()
{
    Serial.begin(115200);
    Serial0.begin(115200);
    delay(500);
    Serial.println("Starting AMap Navigation on Ryzobee RootMaker (Arduino)...");
    Serial0.println("Starting AMap Navigation on Ryzobee RootMaker (Arduino)...");
    Serial.printf("[Memory] PSRAM total: %u KB, free: %u KB | Internal SRAM free: %u KB\n",
                  ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024, ESP.getFreeHeap() / 1024);

    board.rootmaker.begin(true, true, true, true, true);
    if (board.rootmaker.lcd) {
        brightness_set(brightness_load(), false);       // the level the rider chose last time
        board.rootmaker.lcd->fillScreen(0x0F172A);
    }

    rootmaker_display_init();

    phone_gps_start(on_phone_gps_fix, on_phone_route, on_phone_guide, on_phone_clear);

    if (rootmaker_display_lock(500)) {
        lv_obj_t *map_scr = lv_obj_create(NULL);   // the map gets its own screen; the launcher owns the others
        if (!map_manager_init(map_scr)) {
            Serial.println("Map manager init failed!");
            Serial0.println("Map manager init failed!");
        }
        launcher_init(map_scr);
        rootmaker_display_unlock();
    }

    Serial.println("AMap Navigation ready on RootMaker! Connect phone to ESP32-Hangzhou-Map (192.168.4.1)");
    Serial0.println("AMap Navigation ready on RootMaker! Connect phone to ESP32-Hangzhou-Map (192.168.4.1)");
}

void loop()
{
    Button_event_t ev = board.rootmaker.btn.update();
    if (ev != BTN_EVENT_NONE && launcher_pet_active()) {
        // pet asleep or blacked out: the first button press only wakes it
        if (rootmaker_display_lock(50)) {
            launcher_wake();
            rootmaker_display_unlock();
        }
    } else if (!launcher_map_active()) {
        // home / QR screens: the button does nothing
    } else if (ev == BTN_EVENT_SHORT_PRESS) {
        // Physical button short press: Cycle zoom level
        if (rootmaker_display_lock(50)) {
            int cur_z = map_manager_get_zoom();
            if (cur_z >= 20) {
                // Loop back to 15
                for (int i = 0; i < 5; i++) map_manager_zoom_out();
            } else {
                map_manager_zoom_in();
            }
            rootmaker_display_unlock();
        }
    } else if (ev == BTN_EVENT_LONG_PRESS) {
        // Physical button long press: Re-center on GPS
        if (rootmaker_display_lock(50)) {
            map_manager_center_on_gps();
            rootmaker_display_unlock();
        }
    }

    delay(20);
}
