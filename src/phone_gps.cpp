#include "phone_gps.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "amap_companion_html.h"
#include "coord_cn.h"

#define WIFI_AP_SSID       "ESP32-Hangzhou-Map"
/* The phone's hotspot (the board joins it as a station). Kept out of git: set HOTSPOT_SSID / HOTSPOT_PASSWORD
 * in include/secrets.h (template: include/secrets.example.h). */
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef HOTSPOT_SSID
#define HOTSPOT_SSID ""
#endif
#ifndef HOTSPOT_PASSWORD
#define HOTSPOT_PASSWORD ""
#endif
#define WIFI_STA_SSID      HOTSPOT_SSID
#define WIFI_STA_PASS      HOTSPOT_PASSWORD

static phone_gps_fix_t s_latest_fix = {0};
static phone_gps_cb_t s_fix_cb = NULL;
static phone_route_cb_t s_route_cb = NULL;
static phone_guide_cb_t s_guide_cb = NULL;
static phone_clear_cb_t s_clear_cb = NULL;
static WebServer s_server(80);
static bool s_sta_connected = false;
static uint32_t s_contact_ms = 0;   // last call from the phone of any kind
static uint32_t s_feeder_ms = 0;    // last fix from the background GPS feeder (Termux); 0 = never
static volatile uint32_t s_sim_ms = 0;      // last fix of a simulated ride ("sim":true); 0 = none
static volatile bool     s_sim_stop = false; // the rider stopped the simulation on the unit: tell the phone
#define SIM_OWNS_MS 3000
#define FEEDER_OWNS_MS 5000

/** strncpy that never leaves half a UTF-8 character at the end. */
static void copy_utf8(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n > size - 1) {
        n = size - 1;
        while (n > 0 && (((uint8_t)src[n]) & 0xC0) == 0x80) n--;   // back up to a character start
    }
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

static void handle_root()
{
    s_server.sendHeader("Content-Encoding", "gzip");
    s_server.send_P(200, "text/html", (const char *)AMAP_COMPANION_HTML_GZ, AMAP_COMPANION_HTML_GZ_LEN);
}

static void handle_gps()
{
    s_contact_ms = millis() | 1;
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"error\":\"Missing body\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s_server.arg("plain"));
    if (err) {
        s_server.send(400, "application/json", "{\"error\":\"JSON parse failed\"}");
        return;
    }

    /* Two possible GPS sources: the page (only while the phone screen is on) and the Termux
     * feeder (keeps running with the screen off). While the feeder is alive it owns the
     * position; the page's own fixes would only make the arrow fight itself. */
    /* Simulated ride (video / demo): its fixes own the position, real GPS from the phone is ignored meanwhile.
     * After the rider stopped it here, the next reply tells the phone to stop too. */
    bool sim = doc["sim"] | false;
    if (sim) {
        if (s_sim_stop) {
            s_server.send(200, "application/json", "{\"status\":\"ok\",\"stop\":true}");
            return;
        }
        s_sim_ms = millis() | 1;
    } else if (s_sim_ms && (millis() - s_sim_ms) < SIM_OWNS_MS) {
        s_server.send(200, "application/json", "{\"status\":\"sim\"}");
        return;
    }

    const char *src = doc["src"] | "";
    if (sim) {
        /* handled above */
    } else if (src[0] == 't') {
        s_feeder_ms = millis() | 1;
    } else if (s_feeder_ms && (millis() - s_feeder_ms) < FEEDER_OWNS_MS) {
        s_server.send(200, "application/json", "{\"status\":\"feeder\"}");
        return;
    }

    double lat = doc["lat"] | 0.0;
    double lon = doc["lon"] | 0.0;
    if (doc["wgs"] | false) wgs84_to_gcj02(lat, lon, &lat, &lon);   // raw GPS -> AMap coordinates

    s_latest_fix.lat = lat;
    s_latest_fix.lon = lon;
    if (!doc["heading"].isNull()) s_latest_fix.heading_deg = doc["heading"] | 0.0;   // absent while standing still: keep the last
    s_latest_fix.speed_mps = (doc["speed"] | 0.0) / 3.6;   // km/h on the wire
    s_latest_fix.valid = true;
    s_latest_fix.last_update_ms = millis();

    if (s_fix_cb) {
        s_fix_cb(&s_latest_fix);
    }

    if ((doc["nav"] | false) && s_guide_cb) {
        phone_guide_t g;
        memset(&g, 0, sizeof(g));
        g.speed_kmh = (int)lround(doc["speed"] | 0.0);

        JsonObjectConst gd = doc["guide"];
        if (!gd.isNull()) {
            g.has_banner = true;
            const char *k = gd["k"] | "straight";
            if (!strcmp(k, "turn"))        g.kind = PHONE_GUIDE_TURN;
            else if (!strcmp(k, "uturn"))  g.kind = PHONE_GUIDE_UTURN;
            else if (!strcmp(k, "arrive")) g.kind = PHONE_GUIDE_ARRIVE;
            else if (!strcmp(k, "ring"))   g.kind = PHONE_GUIDE_REROUTE;
            else                           g.kind = PHONE_GUIDE_STRAIGHT;
            g.angle_deg = gd["d"] | 0;
            copy_utf8(g.num,    sizeof(g.num),    gd["n"] | "");
            copy_utf8(g.unit,   sizeof(g.unit),   gd["u"] | "");
            copy_utf8(g.action, sizeof(g.action), gd["a"] | "");
            copy_utf8(g.prefix, sizeof(g.prefix), gd["p"] | "");
            copy_utf8(g.road,   sizeof(g.road),   gd["r"] | "");
        }

        JsonObjectConst info = doc["info"];
        if (!info.isNull()) {
            g.has_info = true;
            g.remain_m = info["rm"] | 0;
            g.remain_s = info["rs"] | 0;
            copy_utf8(g.eta, sizeof(g.eta), info["eta"] | "");
        }

        if (g.has_banner || g.has_info) s_guide_cb(&g);
    }

    s_server.send(200, "application/json", "{\"status\":\"ok\"}");
}

/* The Termux feeder scans the hotspot's subnet for this to find the device. */
static void handle_ping()
{
    s_contact_ms = millis() | 1;
    s_server.send(200, "text/plain", "bike-nav");
}

static void handle_clear()
{
    s_contact_ms = millis() | 1;
    s_sim_ms = 0;                           /* (a pending stop stays until the next route, so a late fix cannot revive it) */
    if (s_clear_cb) s_clear_cb();
    s_server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handle_route()
{
    s_contact_ms = millis() | 1;
    s_sim_stop = false;                     /* a new ride (real or simulated) starts clean */
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"error\":\"Missing body\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s_server.arg("plain"));
    if (err) {
        s_server.send(400, "application/json", "{\"error\":\"JSON parse failed\"}");
        return;
    }

    JsonArray wpts = doc["waypoints"];
    if (wpts.isNull() || wpts.size() < 2) {
        s_server.send(400, "application/json", "{\"error\":\"Need at least 2 waypoints\"}");
        return;
    }

    int count = wpts.size();
    double *lats = (double *)malloc(count * sizeof(double));
    double *lons = (double *)malloc(count * sizeof(double));
    if (!lats || !lons) {
        if (lats) free(lats);
        if (lons) free(lons);
        s_server.send(500, "application/json", "{\"error\":\"Out of memory\"}");
        return;
    }

    int valid_pts = 0;
    for (int i = 0; i < count; i++) {
        JsonArray pt = wpts[i];
        if (pt.size() >= 2) {
            lons[valid_pts] = pt[0].as<double>();
            lats[valid_pts] = pt[1].as<double>();
            valid_pts++;
        }
    }

    JsonArray tls = doc["traffic_lights"];
    double *tl_lats = NULL;
    double *tl_lons = NULL;
    int tl_count = 0;
    if (!tls.isNull() && tls.size() > 0) {
        int n = tls.size();
        tl_lats = (double *)malloc(n * sizeof(double));
        tl_lons = (double *)malloc(n * sizeof(double));
        if (tl_lats && tl_lons) {
            for (int i = 0; i < n; i++) {
                JsonArray tl = tls[i];
                if (tl.size() >= 2) {
                    tl_lons[tl_count] = tl[0].as<double>();
                    tl_lats[tl_count] = tl[1].as<double>();
                    tl_count++;
                }
            }
        }
    }

    /* Steps ("st": [[lng, lat, angle, road], ...]) let the device build the banner itself. */
    phone_step_t *steps = NULL;
    int step_count = 0;
    JsonArray sts = doc["st"];
    if (!sts.isNull() && sts.size() > 0) {
        int n = sts.size() > 96 ? 96 : (int)sts.size();
        steps = (phone_step_t *)calloc(n, sizeof(phone_step_t));
        if (steps) {
            for (int i = 0; i < n; i++) {
                JsonArray a = sts[i];
                if (a.size() < 4) continue;
                steps[step_count].lon = a[0].as<double>();
                steps[step_count].lat = a[1].as<double>();
                steps[step_count].angle = a[2] | 0;
                copy_utf8(steps[step_count].road, sizeof(steps[step_count].road), a[3] | "");
                step_count++;
            }
        }
    }

    if (valid_pts >= 2 && s_route_cb) {
        phone_route_t r = {0};
        r.steps = steps;
        r.step_count = step_count;
        const char *name = doc["dest_name"] | "目的地";
        strncpy(r.dest_name, name, sizeof(r.dest_name) - 1);
        r.dest_lat = doc["dest_lat"] | 0.0;
        r.dest_lon = doc["dest_lon"] | 0.0;
        r.lats = lats;
        r.lons = lons;
        r.point_count = valid_pts;
        r.tl_lats = tl_lats;
        r.tl_lons = tl_lons;
        r.tl_count = tl_count;

        s_route_cb(&r);
    }

    if (tl_lats) free(tl_lats);
    if (tl_lons) free(tl_lons);
    if (steps) free(steps);
    free(lats);
    free(lons);

    s_server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void wifi_server_task(void *param)
{
    bool last_sta = false;
    while (1) {
        s_server.handleClient();
        bool cur_sta = (WiFi.status() == WL_CONNECTED);
        if (cur_sta != last_sta) {
            last_sta = cur_sta;
            s_sta_connected = cur_sta;
            if (cur_sta) {
                IPAddress ip = WiFi.localIP();
                Serial.printf("\n[Wi-Fi STA] Connected to '%s'! Assigned IP: %s\n", WIFI_STA_SSID, ip.toString().c_str());
                Serial0.printf("\n[Wi-Fi STA] Connected to '%s'! Assigned IP: %s\n", WIFI_STA_SSID, ip.toString().c_str());
            } else {
                Serial.printf("\n[Wi-Fi STA] Disconnected from '%s'\n", WIFI_STA_SSID);
                Serial0.printf("\n[Wi-Fi STA] Disconnected from '%s'\n", WIFI_STA_SSID);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool phone_gps_start(phone_gps_cb_t gps_cb, phone_route_cb_t route_cb,
                     phone_guide_cb_t guide_cb, phone_clear_cb_t clear_cb)
{
    s_fix_cb = gps_cb;
    s_route_cb = route_cb;
    s_guide_cb = guide_cb;
    s_clear_cb = clear_cb;

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_AP_SSID);
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);

    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/api/gps", HTTP_POST, handle_gps);
    s_server.on("/api/route", HTTP_POST, handle_route);
    s_server.on("/api/clear", HTTP_POST, handle_clear);
    s_server.on("/api/ping", HTTP_GET, handle_ping);
    s_server.begin();

    xTaskCreatePinnedToCore(wifi_server_task, "wifi_server", 4096, NULL, 4, NULL, 0);
    return true;
}

void phone_gps_get_latest(phone_gps_fix_t *out_fix)
{
    if (out_fix) *out_fix = s_latest_fix;
}

bool phone_gps_recent_contact(void)
{
    return s_contact_ms && ((millis() - s_contact_ms) < 10000);
}

bool phone_gps_sim_active(void)
{
    return s_sim_ms && ((millis() - s_sim_ms) < SIM_OWNS_MS);
}

void phone_gps_sim_stop(void)
{
    s_sim_stop = true;
    s_sim_ms = 0;
}

bool phone_gps_is_active(void)
{
    return s_latest_fix.valid && ((millis() - s_latest_fix.last_update_ms) < 5000);
}

bool phone_gps_is_wifi_connected(void)
{
    return s_sta_connected;
}

