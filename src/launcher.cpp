#include "launcher.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#include <math.h>
#include "map_manager.h"
#include "phone_gps.h"
#include "brightness.h"
#include "rider.h"
#include "rootmaker_display.h"      // extern Ryzobee board (the battery chip lives there)

extern const lv_font_t font_chinese_16;
extern const lv_font_t font_chinese_20;

typedef struct {
    const char *name;                   /* label under the icon */
    const char *symbol;                 /* LV_SYMBOL_* drawn in the icon (or NULL: use draw_icon) */
    void      (*draw_icon)(lv_obj_t *icon);   /* custom drawing inside the icon circle */
    uint32_t    color;                  /* icon background, 0xRRGGBB */
    void      (*open)(void);
} launcher_app_t;

/* Idle timing: home screen untouched -> the rider scene appears; untouched on it -> the screen blacks out. */
#define PET_AFTER_MS      3000      /* home screen untouched this long -> idle scene */
#define PET_BLANK_MS      10000     /* idle scene shown this long without a touch -> backlight off */

typedef enum { VIEW_HOME, VIEW_QR, VIEW_MAP, VIEW_BRIGHTNESS, VIEW_PET, VIEW_RIDER } view_t;

static lv_obj_t *s_home_scr = NULL;
static lv_obj_t *s_qr_scr   = NULL;
static lv_obj_t *s_map_scr  = NULL;
static lv_obj_t *s_qr       = NULL;
static lv_obj_t *s_url_lbl  = NULL;
static lv_obj_t *s_home_btn = NULL;
static lv_obj_t *s_bri_scr  = NULL;
static lv_obj_t *s_bri_slider = NULL;
static lv_obj_t *s_bri_label  = NULL;
static lv_obj_t *s_rider_scr = NULL;     /* the "ride" app AND the idle screen */
static uint32_t  s_home_since = 0;      /* when the home screen came up (for the idle test) */
static uint32_t  s_pet_since  = 0;
static bool      s_blank      = false;  /* backlight is off */
static view_t    s_view     = VIEW_HOME;
static char      s_url[48]  = "";
static lv_timer_t *s_demo_timer = NULL;   /* non-NULL while the demo ride runs */

static void show_home(void);
static void show_qr(void);
static void show_map(void);
static void show_brightness(void);
static void show_idle(void);
static void home_ui_hide(void);
static void draw_sun_icon(lv_obj_t *icon);

/* ----------------------------------------------------------------- apps */

static void open_navigation(void)
{
    /* Phone already streaming: straight to the map. Otherwise ask it to connect. */
    if (s_demo_timer || phone_gps_is_active() || phone_gps_recent_contact()) show_map();   /* demo: no QR */
    else show_qr();
}

static const launcher_app_t s_apps[] = {
    { "导航", LV_SYMBOL_GPS, NULL,          0x0284C7, open_navigation },
    { "亮度", NULL,          draw_sun_icon, 0xD97706, show_brightness },
    /* More apps: { "名称", LV_SYMBOL_xxx, NULL, 0xRRGGBB, open_function }, */
};

/* ------------------------------------------------------------------ url */

/** The address the phone should open: the device's IP on the phone's hotspot, or its own AP. */
static void current_url(char *out, size_t n)
{
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(out, n, "http://%s/", WiFi.localIP().toString().c_str());
    } else {
        snprintf(out, n, "http://192.168.4.1/");
    }
}

static void refresh_qr(void)
{
    char url[sizeof(s_url)];
    current_url(url, sizeof(url));
    if (strcmp(url, s_url) == 0) return;

    memcpy(s_url, url, sizeof(s_url));
    lv_qrcode_update(s_qr, s_url, strlen(s_url));
    lv_label_set_text(s_url_lbl, s_url);
}

/* --------------------------------------------------------------- screens */

static void style_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static void battery_update(void);

static void backlight_restore_cb(lv_timer_t *t)
{
    (void)t;
    brightness_set(brightness_load(), false);       /* the level the rider chose */
}

static void show_home(void)
{
    if (s_view == VIEW_PET || s_view == VIEW_RIDER) rider_stop();
    if (s_blank) {
        s_blank = false;
        /* Light up only after the home screen has been drawn, so the last pet frame never flashes. */
        lv_timer_t *t = lv_timer_create(backlight_restore_cb, 80, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
    s_view = VIEW_HOME;
    s_home_since = lv_tick_get();
    battery_update();                                       /* fresh number whenever the home screen comes up */
    map_manager_set_visible(false);
    home_ui_hide();
    lv_screen_load(s_home_scr);
}

static void show_idle(void)
{
    s_view = VIEW_PET;                  /* "pet" = idle scene; kept as the name main.cpp knows */
    s_pet_since = lv_tick_get();
    map_manager_set_visible(false);
    home_ui_hide();
    battery_update();
    rider_start();
    lv_screen_load(s_rider_scr);
}

static void pet_touch_cb(lv_event_t *e)
{
    (void)e;
    if (s_view == VIEW_PET) show_home();            /* any touch (even on the dark screen) wakes it */
}

static void show_qr(void)
{
    s_view = VIEW_QR;
    map_manager_set_visible(false);
    home_ui_hide();
    s_url[0] = 0;               /* force the QR to be redrawn */
    refresh_qr();
    lv_screen_load(s_qr_scr);
}

static void show_map(void)
{
    s_view = VIEW_MAP;
    map_manager_set_visible(true);
    lv_obj_remove_flag(s_home_btn, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(s_map_scr);
}

static void app_clicked_cb(lv_event_t *e)
{
    const launcher_app_t *app = (const launcher_app_t *)lv_event_get_user_data(e);
    if (app && app->open) app->open();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    show_home();
}

static void build_home(void)
{
    s_home_scr = lv_obj_create(NULL);
    style_screen(s_home_scr);
    lv_obj_set_flex_flow(s_home_scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_home_scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_home_scr, 14, 0);
    lv_obj_set_style_pad_gap(s_home_scr, 12, 0);

    for (size_t i = 0; i < sizeof(s_apps) / sizeof(s_apps[0]); i++) {
        lv_obj_t *cell = lv_obj_create(s_home_scr);
        lv_obj_set_size(cell, 84, 96);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_pad_gap(cell, 6, 0);
        lv_obj_set_style_opa(cell, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(cell, app_clicked_cb, LV_EVENT_CLICKED, (void *)&s_apps[i]);

        lv_obj_t *icon = lv_obj_create(cell);
        lv_obj_set_size(icon, 64, 64);
        lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(s_apps[i].color), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);      /* the cell takes the tap */

        if (s_apps[i].draw_icon) {
            s_apps[i].draw_icon(icon);
        } else {
            lv_obj_t *sym = lv_label_create(icon);
            lv_label_set_text(sym, s_apps[i].symbol);
            lv_obj_set_style_text_font(sym, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(sym, lv_color_white(), 0);
            lv_obj_center(sym);
        }

        lv_obj_t *name = lv_label_create(cell);
        lv_label_set_text(name, s_apps[i].name);
        lv_obj_set_style_text_font(name, &font_chinese_16, 0);
        lv_obj_set_style_text_color(name, lv_color_white(), 0);
    }
}

static void build_qr(void)
{
    s_qr_scr = lv_obj_create(NULL);
    style_screen(s_qr_scr);

    lv_obj_t *title = lv_label_create(s_qr_scr);
    lv_label_set_text(title, "手机扫码连接");
    lv_obj_set_style_text_font(title, &font_chinese_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* White card = the quiet zone scanners need around a QR code. */
    lv_obj_t *card = lv_obj_create(s_qr_scr);
    lv_obj_set_size(card, 164, 164);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_qr = lv_qrcode_create(card);
    lv_qrcode_set_size(s_qr, 150);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_center(s_qr);

    s_url_lbl = lv_label_create(s_qr_scr);
    lv_label_set_text(s_url_lbl, "");
    lv_obj_set_style_text_font(s_url_lbl, &font_chinese_16, 0);
    lv_obj_set_style_text_color(s_url_lbl, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(s_url_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_t *back = lv_button_create(s_qr_scr);
    lv_obj_set_size(back, 34, 34);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1E293B), 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
}

/* Floating "home" button on the map. A LONG press goes back to the app grid (the route keeps running), so a
 * bump on the road cannot kick you out. A plain tap only shows a hint, because that is not obvious. */
static lv_obj_t *s_home_hint = NULL;

static void home_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (phone_gps_sim_active()) {           /* simulated ride: leaving the map stops it (the phone follows) */
        phone_gps_sim_stop();
        map_manager_clear_route();
    }
    show_home();
}

static void hint_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_home_hint) lv_obj_add_flag(s_home_hint, LV_OBJ_FLAG_HIDDEN);
}

static void home_tap_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_remove_flag(s_home_hint, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(hint_hide_cb, 1800, NULL);
    lv_timer_set_repeat_count(t, 1);                      /* one shot, deletes itself */
}

static void home_ui_hide(void)
{
    lv_obj_add_flag(s_home_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_home_hint) lv_obj_add_flag(s_home_hint, LV_OBJ_FLAG_HIDDEN);
}

static void build_home_button(void)
{
    s_home_btn = lv_button_create(lv_layer_top());
    lv_obj_set_size(s_home_btn, 40, 40);                     /* easy to hit with a finger */
    lv_obj_align(s_home_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(s_home_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_home_btn, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_home_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_home_btn, 1, 0);
    lv_obj_set_style_border_color(s_home_btn, lv_color_hex(0x334155), 0);
    lv_obj_add_event_cb(s_home_btn, home_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_home_btn, home_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_flag(s_home_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *l = lv_label_create(s_home_btn);
    lv_label_set_text(l, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(l, lv_color_hex(0x38BDF8), 0);
    lv_obj_center(l);

    s_home_hint = lv_label_create(lv_layer_top());
    lv_label_set_text(s_home_hint, "长按回主页");
    lv_obj_set_style_text_font(s_home_hint, &font_chinese_16, 0);
    lv_obj_set_style_text_color(s_home_hint, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_home_hint, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_home_hint, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_home_hint, 10, 0);
    lv_obj_set_style_pad_hor(s_home_hint, 10, 0);
    lv_obj_set_style_pad_ver(s_home_hint, 4, 0);
    lv_obj_align(s_home_hint, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_add_flag(s_home_hint, LV_OBJ_FLAG_HIDDEN);
}

/* -------------------------------------------------------------- battery */

typedef struct { lv_obj_t *box, *fill, *lbl, *bolt; } bat_ui_t;
static bat_ui_t s_bat[2];
static int      s_bat_n = 0;
static bool     s_bat_charging = false;

#define BAT_BODY_W   28
#define BAT_BODY_H   14
#define BAT_FILL_MAX (BAT_BODY_W - 4)       /* inside the 2 px outline */

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* A small battery (outline, level bar, tip) with the percentage beside it. One per screen that shows it. */
static void build_battery(lv_obj_t *parent, lv_align_t align, int x, int y)
{
    if (s_bat_n >= 2) return;
    bat_ui_t &u = s_bat[s_bat_n++];
    u.box = plain_obj(parent);
    lv_obj_add_flag(u.box, LV_OBJ_FLAG_FLOATING);               /* not one of the app icons: stays out of the grid */
    lv_obj_set_size(u.box, 84, 22);
    lv_obj_align(u.box, align, x, y);

    lv_obj_t *body = plain_obj(u.box);
    lv_obj_set_size(body, BAT_BODY_W, BAT_BODY_H);
    lv_obj_align(body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_radius(body, 4, 0);

    u.fill = plain_obj(body);
    lv_obj_set_style_bg_opa(u.fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(u.fill, 2, 0);
    lv_obj_set_size(u.fill, BAT_FILL_MAX, BAT_BODY_H - 4);
    lv_obj_align(u.fill, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *tip = plain_obj(u.box);
    lv_obj_set_size(tip, 3, 6);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tip, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_radius(tip, 1, 0);
    lv_obj_align_to(tip, body, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    u.lbl = lv_label_create(u.box);
    lv_obj_set_style_text_font(u.lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(u.lbl, lv_color_white(), 0);
    lv_label_set_text(u.lbl, "--%");
    lv_obj_align_to(u.lbl, tip, LV_ALIGN_OUT_RIGHT_MID, 5, 0);

    u.bolt = lv_label_create(u.box);                            /* the charging bolt, over the battery */
    lv_obj_set_style_text_font(u.bolt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(u.bolt, lv_color_white(), 0);
    lv_label_set_text(u.bolt, LV_SYMBOL_CHARGE);
    lv_obj_align_to(u.bolt, body, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(u.bolt, LV_OBJ_FLAG_HIDDEN);
}

/** Warm the chip's voltage filter so the first number is not a single noisy sample. */
static void battery_warm_up(void)
{
    for (int i = 0; i < 10; i++) board.rootmaker.bat.updateVoltageFilter();
}

static void battery_update(void)
{
    if (s_bat_n == 0) return;
    Rootmaker_bat &bat = board.rootmaker.bat;
    if (!bat.isConnected()) {                               /* no power chip answering: show nothing */
        for (int i = 0; i < s_bat_n; i++) lv_obj_add_flag(s_bat[i].box, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    bool charging = bat.isCharging();
    if (charging != s_bat_charging) {                       /* the voltage jumps when a charger is (un)plugged */
        s_bat_charging = charging;
        bat.resetVoltageFilter();
        battery_warm_up();
    }

    int pct = bat.getBatteryLevel();
    if (bat.isChargeFull()) pct = 100;
    if (pct > 100) pct = 100;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int w = (BAT_FILL_MAX * pct) / 100;
    if (pct > 0 && w < 2) w = 2;
    uint32_t color = charging ? 0x22C55E : (pct <= 20 ? 0xEF4444 : (pct <= 40 ? 0xFBBF24 : 0x22C55E));
    for (int i = 0; i < s_bat_n; i++) {
        bat_ui_t &u = s_bat[i];
        lv_obj_remove_flag(u.box, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(u.lbl, buf);
        lv_obj_set_width(u.fill, w);
        lv_obj_set_style_bg_color(u.fill, lv_color_hex(color), 0);
        if (charging) lv_obj_remove_flag(u.bolt, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(u.bolt, LV_OBJ_FLAG_HIDDEN);
    }
}

static void battery_timer_cb(lv_timer_t *t)
{
    (void)t;
    battery_update();
}

/* ---------------------------------------------------------- brightness */

/* A sun: a disc with eight rays, drawn from shapes so no icon font is needed. */
static void draw_sun_icon(lv_obj_t *icon)
{
    static lv_point_precise_t rays[8][2];
    for (int i = 0; i < 8; i++) {
        float a = i * (float)M_PI / 4.0f;
        rays[i][0].x = (lv_value_precise_t)lroundf(32 + 17 * sinf(a));
        rays[i][0].y = (lv_value_precise_t)lroundf(32 - 17 * cosf(a));
        rays[i][1].x = (lv_value_precise_t)lroundf(32 + 25 * sinf(a));
        rays[i][1].y = (lv_value_precise_t)lroundf(32 - 25 * cosf(a));
        lv_obj_t *l = lv_line_create(icon);
        lv_line_set_points(l, rays[i], 2);
        lv_obj_set_style_line_width(l, 3, 0);
        lv_obj_set_style_line_color(l, lv_color_white(), 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_set_pos(l, 0, 0);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *disc = lv_obj_create(icon);
    lv_obj_set_size(disc, 22, 22);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(disc, 0, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(disc);
}

static void bri_show(int pct)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_bri_label, buf);
}

static void bri_slider_cb(lv_event_t *e)
{
    int pct = lv_slider_get_value(s_bri_slider);
    bri_show(pct);
    brightness_set(pct, lv_event_get_code(e) == LV_EVENT_RELEASED);   /* live while dragging, saved on release */
}

static void bri_step_cb(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int pct = lv_slider_get_value(s_bri_slider) + step;
    if (pct < BRIGHTNESS_MIN_PCT) pct = BRIGHTNESS_MIN_PCT;
    if (pct > 100) pct = 100;
    lv_slider_set_value(s_bri_slider, pct, LV_ANIM_OFF);
    bri_show(pct);
    brightness_set(pct, true);
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, int step, lv_align_t align, int x, int y)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 52, 52);
    lv_obj_align(b, align, x, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E293B), 0);
    lv_obj_add_event_cb(b, bri_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)step);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    return b;
}

/* ------------------------------------------------------------------ demo */

/* Demo ride: starts at West Lake (Broken Bridge, Hangzhou)
 * and rides 10 km due north in 5 m steps every 0.5 s (36 km/h), feeding the map exactly like real GPS fixes.
 * The Demo button is green while it runs, grey otherwise; tapping it while it runs stops it. */
#define DEMO_DISTANCE_M 10000.0
#define DEMO_STEP_M     5.0
#define DEMO_PERIOD_MS  500
#define M_PER_DEG_LAT   111320.0
#define DEMO_START_LAT  30.2596     /* "me" in the demo: Broken Bridge, on the shore of West Lake, Hangzhou (GCJ-02) */
#define DEMO_START_LON  120.1479

static double      s_demo_lat, s_demo_lon, s_demo_done_m;
static lv_obj_t   *s_demo_btn = NULL;

static void demo_button_update(void)
{
    if (s_demo_btn) lv_obj_set_style_bg_color(s_demo_btn, lv_color_hex(s_demo_timer ? 0x16A34A : 0x475569), 0);
}

static void demo_cb(lv_timer_t *t)
{
    s_demo_done_m += DEMO_STEP_M;
    s_demo_lat += DEMO_STEP_M / M_PER_DEG_LAT;
    map_manager_update_gps(s_demo_lat, s_demo_lon, 0.0, DEMO_STEP_M * 1000.0 / DEMO_PERIOD_MS);
    if (s_demo_done_m >= DEMO_DISTANCE_M) {
        lv_timer_delete(t);
        s_demo_timer = NULL;
        demo_button_update();
    }
}

static void demo_start_cb(lv_event_t *e)
{
    (void)e;
    if (s_demo_timer) {                                 /* running: this tap stops it */
        lv_timer_delete(s_demo_timer);
        s_demo_timer = NULL;
        demo_button_update();
        return;
    }
    s_demo_lon = DEMO_START_LON;
    s_demo_lat = DEMO_START_LAT;                                  /* "me" starts right at West Lake */
    s_demo_done_m = 0;
    s_demo_timer = lv_timer_create(demo_cb, DEMO_PERIOD_MS, NULL);
    demo_button_update();
    map_manager_update_gps(s_demo_lat, s_demo_lon, 0.0, 0.0);   /* show the start point right away */
    show_map();
}

static void build_brightness(void)
{
    s_bri_scr = lv_obj_create(NULL);
    style_screen(s_bri_scr);

    lv_obj_t *title = lv_label_create(s_bri_scr);
    lv_label_set_text(title, "屏幕亮度");
    lv_obj_set_style_text_font(title, &font_chinese_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *back = lv_button_create(s_bri_scr);
    lv_obj_set_size(back, 40, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1E293B), 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    s_bri_label = lv_label_create(s_bri_scr);
    lv_obj_set_style_text_font(s_bri_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_bri_label, lv_color_white(), 0);
    lv_obj_align(s_bri_label, LV_ALIGN_CENTER, 0, -38);

    s_bri_slider = lv_slider_create(s_bri_scr);
    lv_obj_set_size(s_bri_slider, 190, 22);
    lv_obj_align(s_bri_slider, LV_ALIGN_CENTER, 0, 6);
    lv_slider_set_range(s_bri_slider, BRIGHTNESS_MIN_PCT, 100);
    lv_obj_set_style_bg_color(s_bri_slider, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bri_slider, lv_color_hex(0xFBBF24), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bri_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_bri_slider, 8, LV_PART_KNOB);           /* a knob big enough for a finger */
    lv_obj_add_event_cb(s_bri_slider, bri_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bri_slider, bri_slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *demo = lv_button_create(s_bri_scr);
    lv_obj_set_size(demo, 76, 40);
    lv_obj_align(demo, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_radius(demo, 20, 0);
    s_demo_btn = demo;
    demo_button_update();
    lv_obj_add_event_cb(demo, demo_start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(demo);
    lv_label_set_text(dl, "Demo");
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
    lv_obj_center(dl);

    round_button(s_bri_scr, LV_SYMBOL_MINUS, -10, LV_ALIGN_BOTTOM_LEFT, 40, -22);
    round_button(s_bri_scr, LV_SYMBOL_PLUS, 10, LV_ALIGN_BOTTOM_RIGHT, -40, -22);
}

static void show_brightness(void)
{
    s_view = VIEW_BRIGHTNESS;
    map_manager_set_visible(false);
    home_ui_hide();
    int pct = brightness_load();
    lv_slider_set_value(s_bri_slider, pct, LV_ANIM_OFF);
    bri_show(pct);
    demo_button_update();
    lv_screen_load(s_bri_scr);
}

/* While the QR is up: keep it current and open the map the moment the phone connects. */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_view == VIEW_HOME) {
        /* Untouched for 3 s (and the screen has been up that long, e.g. after a button wake) -> the pet. */
        if (lv_display_get_inactive_time(NULL) >= PET_AFTER_MS && lv_tick_elaps(s_home_since) >= PET_AFTER_MS) show_idle();
        return;
    }
    if (s_view == VIEW_PET) {
        uint32_t shown = lv_tick_elaps(s_pet_since);
        if (!s_blank && shown >= PET_BLANK_MS) {
            s_blank = true;
            rider_stop();
            brightness_off();
        }
        return;
    }
    if (s_view != VIEW_QR) return;
    refresh_qr();
    if (phone_gps_is_active() || phone_gps_recent_contact()) show_map();
}

void launcher_init(lv_obj_t *map_screen)
{
    s_map_scr = map_screen;
    build_home();
    build_battery(s_home_scr, LV_ALIGN_TOP_LEFT, 8, 6);
    battery_warm_up();
    lv_timer_create(battery_timer_cb, 10000, NULL);
    build_qr();
    build_brightness();
    build_home_button();
    s_rider_scr = rider_init();
    {
        lv_obj_t *back = lv_button_create(s_rider_scr);
        lv_obj_set_size(back, 36, 36);
        lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x1E293B), 0);
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(back);
        lv_label_set_text(bl, LV_SYMBOL_LEFT);
        lv_obj_center(bl);
    }
    build_battery(s_rider_scr, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_add_event_cb(s_rider_scr, pet_touch_cb, LV_EVENT_PRESSED, NULL);   /* touch wakes from idle */
    lv_timer_create(tick_cb, 200, NULL);
    show_home();
}

bool launcher_map_active(void)
{
    return s_view == VIEW_MAP;
}

void launcher_wake(void)
{
    if (s_view == VIEW_PET) show_home();
}

bool launcher_pet_active(void)
{
    return s_view == VIEW_PET;
}
