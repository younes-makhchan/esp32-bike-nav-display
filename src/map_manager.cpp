#include "map_manager.h"
#include "map_tiles/include/map_view.h"
#include "map_tiles/include/map_tile_cache.h"
#include "map_tiles/include/map_geo.h"
#include "map_tiles/include/map_route.h"
#include "nav_guide.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <Arduino.h>
#include "esp_heap_caps.h"

extern const lv_font_t font_chinese_16;
extern const lv_font_t font_chinese_20;

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 240

#define MIN_ZOOM 11
#define MAX_ZOOM 20
#define HZ_DEFAULT_ZOOM 18

#define HZ_DEFAULT_LAT 30.250000
#define HZ_DEFAULT_LON 120.150000

/* Tile cache: 128 KB per slot in PSRAM. Take ~35% of what is free at boot, so the
 * route, sprites and WiFi keep their room; 6 is the floor (visible tiles + a spare). */
#define CACHE_MIN_SLOTS 6
#define CACHE_MAX_SLOTS 24

static int pick_cache_capacity(void)
{
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int slots = (int)(free_psram * 35 / 100 / (256 * 256 * 2));
    if (slots < CACHE_MIN_SLOTS) slots = CACHE_MIN_SLOTS;
    if (slots > CACHE_MAX_SLOTS) slots = CACHE_MAX_SLOTS;
    Serial.printf("[Map] PSRAM free %u KB -> tile cache %d slots\n", (unsigned)(free_psram / 1024), slots);
    return slots;
}

static lv_obj_t *s_map_view = NULL;

/* ---- heading-up map while navigating: the road ahead is always at the top ---- */
#define ROT_LOOKAHEAD_PX   70       /* aim at the route this far ahead on screen, so the map turns before the corner */
#define ROT_MIN_SPEED_KMH  4        /* below this the GPS course is noise: keep the last direction */
#define ROT_DEADBAND_DEG   1.5      /* smaller changes are not worth redrawing every tile */
#define ROT_PERIOD_MS      100
static double      s_rot_target = 0.0;      /* wanted map rotation (= -bearing of travel) */
static bool        s_rot_have   = false;    /* a direction is known for this ride */
static lv_timer_t *s_rot_timer  = NULL;

static double wrap180(double a)
{
    while (a > 180.0) a -= 360.0;
    while (a <= -180.0) a += 360.0;
    return a;
}

/* Turn the map a fraction of the way to the target each tick: smooth, shortest way round. */
static void rot_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_map_view || !s_rot_have) return;
    const double cur = map_view_get_rotation(s_map_view);
    const double diff = wrap180(s_rot_target - cur);
    if (fabs(diff) < ROT_DEADBAND_DEG) return;
    double step = diff * 0.3;
    if (fabs(step) < 1.0) step = diff > 0 ? 1.0 : -1.0;
    map_view_set_rotation(s_map_view, cur + step);
}
static map_route_handle_t s_active_route = NULL;

static lv_obj_t *s_status_card = NULL;
static lv_obj_t *s_turn_icon_canvas = NULL;
static lv_draw_buf_t *s_turn_canvas_draw_buf = NULL;

static lv_obj_t *s_top_row = NULL;
static lv_obj_t *s_lbl_dist_val = NULL;
static lv_obj_t *s_lbl_dist_unit = NULL;
static lv_obj_t *s_lbl_dist_action = NULL;

static lv_obj_t *s_bot_row = NULL;
static lv_obj_t *s_lbl_road_prefix = NULL;
static lv_obj_t *s_lbl_road_name = NULL;

static lv_obj_t *s_zoom_bar = NULL;
static lv_obj_t *s_zoom_in_btn = NULL;
static lv_obj_t *s_zoom_out_btn = NULL;
static lv_obj_t *s_zoom_lbl = NULL;
static lv_obj_t *s_center_btn = NULL;

static void update_zoom_ui(void)
{
    if (!s_map_view || !s_zoom_lbl) return;
    int z = map_view_get_zoom(s_map_view);
    char buf[8];
    snprintf(buf, sizeof(buf), "Z%d", z);
    lv_label_set_text(s_zoom_lbl, buf);
}

static lv_obj_t *s_speed_badge = NULL;
static int       s_speed_kmh = 0;
static bool      s_speed_on = false;     /* the phone is navigating, so the badge has a value */
static lv_obj_t *s_lbl_speed = NULL;

/* The turn arrow is as tall as the two text rows beside it. Measured from the real fonts at start-up
 * (see measure_text_block), so it follows the fonts instead of a guessed number. */
static int s_icon_px = 48;
#define ICON_PX (s_icon_px)

#define ROW1_Y 4        /* top text row: distance / action */
#define ROW1_H 28
#define ROW2_Y 34       /* bottom text row: road name */
#define ROW2_H 22
static int s_icon_key = -1;

static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void set_shown(lv_obj_t *o, bool show)
{
    if (!o) return;
    if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* The turn card only exists while there is a route: with nowhere to go there is no direction to show. */
static bool s_card_on = false;
static bool s_map_visible = true;

static void card_show(bool on)
{
    s_card_on = on;
    set_shown(s_status_card, on && s_map_visible);
}

/* ---- turn icon: same shapes as the phone banner (100x100 design grid) ---- */

/* Every arrow is first described in a 100x100 design grid, then scaled so its ink fills the icon's full height
 * (or its full width, for a shape wider than tall). That makes a turn arrow exactly as tall as the straight one,
 * however the shape sits inside the grid. */
#define ICON_STROKE 11.0f          /* stroke width in the design grid */

typedef struct { float x1, y1, x2, y2; } shape_seg_t;
typedef struct { float x[3], y[3]; } shape_tri_t;

static struct {
    shape_seg_t seg[16];
    int         nseg;
    shape_tri_t tri[6];
    int         ntri;
} s_shape;

static void shape_reset(void) { s_shape.nseg = 0; s_shape.ntri = 0; }

static void shape_seg(float x1, float y1, float x2, float y2)
{
    if (s_shape.nseg < 16) s_shape.seg[s_shape.nseg++] = { x1, y1, x2, y2 };
}

static void shape_tri(float x0, float y0, float x1, float y1, float x2, float y2)
{
    if (s_shape.ntri < 6) s_shape.tri[s_shape.ntri++] = { { x0, x1, x2 }, { y0, y1, y2 } };
}

static void shape_mirror(void)
{
    for (int i = 0; i < s_shape.nseg; i++) { s_shape.seg[i].x1 = 100.0f - s_shape.seg[i].x1; s_shape.seg[i].x2 = 100.0f - s_shape.seg[i].x2; }
    for (int i = 0; i < s_shape.ntri; i++) for (int k = 0; k < 3; k++) s_shape.tri[i].x[k] = 100.0f - s_shape.tri[i].x[k];
}

/** Draw the recorded shape scaled to fill the icon. */
static void shape_draw(lv_layer_t *layer)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    const float hw = ICON_STROKE / 2.0f;
    for (int i = 0; i < s_shape.nseg; i++) {
        const shape_seg_t &g = s_shape.seg[i];
        minx = fminf(minx, fminf(g.x1, g.x2) - hw); maxx = fmaxf(maxx, fmaxf(g.x1, g.x2) + hw);
        miny = fminf(miny, fminf(g.y1, g.y2) - hw); maxy = fmaxf(maxy, fmaxf(g.y1, g.y2) + hw);
    }
    for (int i = 0; i < s_shape.ntri; i++) {
        for (int k = 0; k < 3; k++) {
            minx = fminf(minx, s_shape.tri[i].x[k]); maxx = fmaxf(maxx, s_shape.tri[i].x[k]);
            miny = fminf(miny, s_shape.tri[i].y[k]); maxy = fmaxf(maxy, s_shape.tri[i].y[k]);
        }
    }
    const float w = maxx - minx, h = maxy - miny;
    if (w <= 0 || h <= 0) return;
    const float sc = ICON_PX / fmaxf(w, h);
    const float ox = (ICON_PX - w * sc) / 2.0f - minx * sc;
    const float oy = (ICON_PX - h * sc) / 2.0f - miny * sc;

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_white();
    d.opa = LV_OPA_COVER;
    d.width = (int32_t)lroundf(ICON_STROKE * sc);
    if (d.width < 3) d.width = 3;
    d.round_start = 1;
    d.round_end = 1;
    for (int i = 0; i < s_shape.nseg; i++) {
        const shape_seg_t &g = s_shape.seg[i];
        d.p1.x = (lv_value_precise_t)lroundf(g.x1 * sc + ox); d.p1.y = (lv_value_precise_t)lroundf(g.y1 * sc + oy);
        d.p2.x = (lv_value_precise_t)lroundf(g.x2 * sc + ox); d.p2.y = (lv_value_precise_t)lroundf(g.y2 * sc + oy);
        lv_draw_line(layer, &d);
    }
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.bg_color = lv_color_white();
    t.bg_opa = LV_OPA_COVER;
    for (int i = 0; i < s_shape.ntri; i++) {
        for (int k = 0; k < 3; k++) {
            t.p[k].x = (lv_value_precise_t)lroundf(s_shape.tri[i].x[k] * sc + ox);
            t.p[k].y = (lv_value_precise_t)lroundf(s_shape.tri[i].y[k] * sc + oy);
        }
        lv_draw_triangle(layer, &t);
    }
}

/**
 * Where the text block's ink starts and ends, in card coordinates: from the top of the digits in row 1 to the
 * bottom of the Chinese characters in row 2. The arrow gets exactly this top and height.
 */
static void measure_text_block(int *ink_top, int *ink_height)
{
    const lv_font_t *f24 = &lv_font_montserrat_24;
    const lv_font_t *f16 = &font_chinese_16;
    const int lh24 = lv_font_get_line_height(f24);
    const int lh16 = lv_font_get_line_height(f16);

    int top24 = lh24;                                       /* topmost ink of "0".."9" inside its line */
    for (const char *p = "0123456789"; *p; p++) {
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f24, &g, (uint32_t)*p, 0)) continue;
        int y = lh24 - f24->base_line - (int)g.box_h - (int)g.ofs_y;
        if (y < top24) top24 = y;
    }

    int bottom16 = 0;                                       /* lowest ink of typical road-name characters */
    static const uint32_t cjk[] = { 0x8FDB, 0x5165, 0x6CBF, 0x8DEF, 0x5F53, 0x524D, 0x76EE, 0x7684 };
    for (uint32_t ch : cjk) {
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f16, &g, ch, 0)) continue;
        int y = lh16 - f16->base_line - (int)g.box_h - (int)g.ofs_y + (int)g.box_h;
        if (y > bottom16) bottom16 = y;
    }

    int row1_label_top = ROW1_Y + (ROW1_H - lh24) / 2;      /* labels are centred in their rows */
    int row2_label_top = ROW2_Y + (ROW2_H - lh16) / 2;
    int top = row1_label_top + top24;
    int bottom = row2_label_top + bottom16;
    int h = bottom - top;
    if (h < 36) h = 36;
    if (h > 54) h = 54;
    *ink_top = top;
    *ink_height = h;
    Serial.printf("[UI] text block ink y=%d..%d -> arrow %d px\n", top, bottom, h);
}

static void render_turn_icon(int kind, int angle)
{
    if (!s_turn_icon_canvas || !s_turn_canvas_draw_buf) return;

    lv_canvas_fill_bg(s_turn_icon_canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(s_turn_icon_canvas, &layer);

    if (kind == PHONE_GUIDE_REROUTE) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.opa = LV_OPA_COVER;
        arc.width = 6;
        arc.rounded = 1;
        arc.center.x = ICON_PX / 2;
        arc.center.y = ICON_PX / 2;
        arc.radius = (ICON_PX - 6) / 2;                     /* the ring fills the icon */
        arc.start_angle = 0;
        arc.end_angle = 270;
        lv_draw_arc(&layer, &arc);
    } else {
        shape_reset();
        if (kind == PHONE_GUIDE_ARRIVE) {
            shape_seg(30, 92, 30, 12);                                  /* pole */
            shape_tri(34, 14, 78, 14, 78, 48);                          /* flag */
            shape_tri(34, 14, 78, 48, 34, 48);
        } else if (kind == PHONE_GUIDE_UTURN) {
            shape_seg(68, 92, 68, 44);
            float px = 68, py = 44;
            for (int a = 30; a <= 180; a += 30) {
                float r = a * (float)M_PI / 180.0f;
                float nx = 51 + 17 * cosf(r), ny = 44 - 17 * sinf(r);
                shape_seg(px, py, nx, ny);
                px = nx; py = ny;
            }
            shape_seg(34, 44, 34, 62);
            shape_tri(34, 86, 18, 60, 50, 60);
            if (angle > 0) shape_mirror();
        } else if (kind == PHONE_GUIDE_TURN) {
            float th = angle * (float)M_PI / 180.0f, sn = sinf(th), cs = cosf(th);
            float L = 34, sx = 50 - 14 * sn, by = (abs(angle) > 100) ? 40 : 54;
            float ex = sx + L * sn, ey = by - L * cs;
            shape_seg(sx, 92, sx, by);
            shape_seg(sx, by, ex, ey);
            shape_tri(ex + 16 * sn, ey - 16 * cs, ex + 14 * cs, ey + 14 * sn, ex - 14 * cs, ey - 14 * sn);
        } else {                                                        /* straight ahead */
            shape_seg(50, 92, 50, 30);
            shape_tri(50, 6, 76, 36, 24, 36);
        }
        shape_draw(&layer);
    }

    lv_canvas_finish_layer(s_turn_icon_canvas, &layer);
}

/* ---- idle / trip info ---- */

static bool s_val_big = false;

static void set_row_fonts(bool big_number, bool action_alone)
{
    if (s_lbl_dist_val && s_val_big != big_number) {
        s_val_big = big_number;
        lv_obj_set_style_text_font(s_lbl_dist_val, big_number ? &lv_font_montserrat_24 : &font_chinese_16, 0);
    }
    if (s_lbl_dist_action) {
        lv_obj_set_style_text_font(s_lbl_dist_action, action_alone ? &font_chinese_20 : &font_chinese_16, 0);
    }
}

static void show_idle(void)
{
    set_row_fonts(false, false);
    s_icon_key = -1;
    render_turn_icon(PHONE_GUIDE_STRAIGHT, 0);

    set_shown(s_lbl_dist_val, true);
    set_shown(s_lbl_dist_unit, true);
    set_text(s_lbl_dist_val, "高德");
    set_text(s_lbl_dist_unit, "骑行");
    set_text(s_lbl_dist_action, "导航");

    set_shown(s_bot_row, true);
    set_shown(s_lbl_road_prefix, true);
    set_text(s_lbl_road_prefix, "等待");
    set_text(s_lbl_road_name, "手机选择目的地");

    s_speed_on = false;
    set_shown(s_speed_badge, false);
    card_show(false);                                       /* idle: no route, no card */
}

static void update_speed(const phone_guide_t *g)
{
    char spd[8];
    snprintf(spd, sizeof(spd), "%d", g->speed_kmh < 0 ? 0 : g->speed_kmh);
    set_text(s_lbl_speed, spd);
    s_speed_on = true;
    set_shown(s_speed_badge, true);
}

void map_manager_set_visible(bool visible)
{
    s_map_visible = visible;
    set_shown(s_status_card, visible && s_card_on);
    set_shown(s_zoom_bar, visible);
    set_shown(s_center_btn, visible);
    set_shown(s_speed_badge, visible && s_speed_on);
}

static void apply_guide(const phone_guide_t *g);

void map_manager_update_gps(double lat, double lon, double heading_deg, double speed_mps)
{
    if (!s_map_view) return;

    /* First fix of the session: put the camera on the rider (the map used to wait
     * for a route before it would follow). */
    static bool s_first_fix = true;
    if (s_first_fix) {
        s_first_fix = false;
        map_view_set_follow(s_map_view, true);
    }
    map_view_set_position(s_map_view, lat, lon, heading_deg, true);

    /* Heading-up while a route is active: the road ahead if we are on it, else the direction of travel. */
    if (s_active_route) {
        double b;
        if (map_view_route_bearing_ahead(s_map_view, ROT_LOOKAHEAD_PX, &b)) {
            s_rot_target = wrap180(-b);
            s_rot_have = true;
        } else if (speed_mps * 3.6 >= ROT_MIN_SPEED_KMH) {
            s_rot_target = wrap180(-heading_deg);
            s_rot_have = true;
        }
    }

    /* Guidance on the device: needs nothing from the phone page, so it keeps
     * working when the phone screen is off. */
    s_speed_kmh = (int)lround(speed_mps * 3.6);
    if (nav_guide_has_steps()) {
        phone_guide_t g;
        if (nav_guide_compute(map_view_get_traveled_m(s_map_view), &g)) {
            g.speed_kmh = s_speed_kmh;
            apply_guide(&g);
        }
    }
}

void map_manager_set_steps(const phone_step_t *steps, int count)
{
    nav_guide_clear();
    if (!s_active_route || !steps || count <= 0) return;

    const int n = map_route_get_point_count(s_active_route);
    static nav_step_t tmp[NAV_GUIDE_MAX_STEPS];
    int out = 0, from = 0;

    for (int i = 0; i < count && out < NAV_GUIDE_MAX_STEPS; i++) {
        /* The phone sent the step's coordinates; find that vertex in the (simplified) route
         * so the distance is in the device's own metric. Steps are in route order. */
        double best = 1e18;
        int bi = from;
        const double kx = cos(steps[i].lat * M_PI / 180.0);
        for (int j = from; j < n; j++) {
            map_route_point_t p;
            if (!map_route_get_point(s_active_route, j, &p)) break;
            double dy = p.lat - steps[i].lat;
            double dx = (p.lon - steps[i].lon) * kx;
            double d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; bi = j; }
        }
        from = bi;

        map_route_point_t p;
        if (!map_route_get_point(s_active_route, bi, &p)) continue;
        tmp[out].dist_m = p.cum_dist_m;
        tmp[out].angle  = (int16_t)steps[i].angle;
        snprintf(tmp[out].road, sizeof(tmp[out].road), "%s", steps[i].road);
        out++;
    }
    nav_guide_set_steps(tmp, out, map_route_get_total_distance_m(s_active_route));
}

static void apply_guide(const phone_guide_t *g)
{
    if (!g || !s_status_card || !s_lbl_dist_val) return;
    card_show(true);

    if (g->has_banner) {
        int key = (int)g->kind * 1000 + g->angle_deg / 5 + 100;
        if (key != s_icon_key) {
            s_icon_key = key;
            render_turn_icon(g->kind, g->angle_deg);
        }

        bool has_num = g->num[0] != 0;
        set_row_fonts(true, !has_num);
        set_shown(s_lbl_dist_val, has_num);
        set_shown(s_lbl_dist_unit, has_num && g->unit[0]);
        set_text(s_lbl_dist_val, g->num);
        set_text(s_lbl_dist_unit, g->unit);
        set_text(s_lbl_dist_action, g->action);

        bool has_road = g->prefix[0] || g->road[0];
        set_shown(s_bot_row, has_road);
        set_shown(s_lbl_road_prefix, g->prefix[0] != 0);
        set_text(s_lbl_road_prefix, g->prefix);
        set_text(s_lbl_road_name, g->road);
    }

    update_speed(g);
}

void map_manager_update_nav(const phone_guide_t *g)
{
    /* With steps, the device computes the banner itself (map_manager_update_gps); the phone's
     * copy would only fight it. Without steps, mirror the phone as before. */
    if (nav_guide_has_steps()) return;
    apply_guide(g);
}

void map_manager_set_route(const double *lats, const double *lons, int point_count, const char *dest_name)
{
    if (!s_map_view) return;

    nav_guide_clear();
    if (s_active_route) {
        map_route_free(s_active_route);
        s_active_route = NULL;
    }

    if (lats && lons && point_count >= 2) {
        s_active_route = map_route_create_from_points(lats, lons, point_count, true);
        if (s_active_route) {
            map_view_set_route(s_map_view, s_active_route);
            map_view_set_follow(s_map_view, true);
            card_show(true);                                /* a route exists: show where to go */

            uint32_t total_m = map_route_get_total_distance_m(s_active_route);
            char dist_buf[32];
            char unit_buf[32];
            if (total_m >= 1000) {
                snprintf(dist_buf, sizeof(dist_buf), "%.1f", total_m / 1000.0);
                snprintf(unit_buf, sizeof(unit_buf), "公里");
            } else {
                snprintf(dist_buf, sizeof(dist_buf), "%u", (unsigned)total_m);
                snprintf(unit_buf, sizeof(unit_buf), "米");
            }

            s_icon_key = -1;
            render_turn_icon(PHONE_GUIDE_STRAIGHT, 0);
            set_row_fonts(true, false);
            set_shown(s_lbl_dist_val, true);
            set_shown(s_lbl_dist_unit, true);
            set_shown(s_bot_row, true);
            set_shown(s_lbl_road_prefix, true);
            if (s_lbl_dist_val) {
                lv_label_set_text(s_lbl_dist_val, dist_buf);
            }
            if (s_lbl_dist_unit) lv_label_set_text(s_lbl_dist_unit, unit_buf);
            if (s_lbl_dist_action) lv_label_set_text(s_lbl_dist_action, "全程");
            if (s_lbl_road_prefix) lv_label_set_text(s_lbl_road_prefix, "目的地");
            if (s_lbl_road_name) lv_label_set_text(s_lbl_road_name, dest_name ? dest_name : "终点");

            s_rot_have = false;                         /* the next fixes decide the direction */
            map_view_set_rotation(s_map_view, 0.0);
        }
    } else {
        s_rot_have = false;
        map_view_set_route(s_map_view, NULL);
        map_view_set_traffic_lights(s_map_view, NULL, NULL, 0);
        map_view_set_rotation(s_map_view, 0.0);
    }
}

void map_manager_set_traffic_lights(const double *lats, const double *lons, int count)
{
    if (!s_map_view) return;
    map_view_set_traffic_lights(s_map_view, lats, lons, count);
}

void map_manager_clear_route(void)
{
    nav_guide_clear();
    if (s_active_route) {
        map_route_free(s_active_route);
        s_active_route = NULL;
    }
    if (s_map_view) {
        map_view_set_route(s_map_view, NULL);
        map_view_set_traffic_lights(s_map_view, NULL, NULL, 0);
        map_view_set_rotation(s_map_view, 0.0);
    }
    s_rot_have = false;                                  /* no route: back to north-up */
    show_idle();
}

void map_manager_set_status_text(const char *text)
{
    if (s_lbl_road_name && text) {
        lv_label_set_text(s_lbl_road_name, text);
    }
}

void map_manager_center_on_gps(void)
{
    if (!s_map_view) return;
    map_view_set_follow(s_map_view, true);
    map_view_orient_to_route(s_map_view);
}

void map_manager_zoom_in(void)
{
    if (!s_map_view) return;
    int z = map_view_get_zoom(s_map_view);
    if (z < MAX_ZOOM) {
        map_view_set_zoom(s_map_view, z + 1);
        update_zoom_ui();
    }
}

void map_manager_zoom_out(void)
{
    if (!s_map_view) return;
    int z = map_view_get_zoom(s_map_view);
    if (z > MIN_ZOOM) {
        map_view_set_zoom(s_map_view, z - 1);
        update_zoom_ui();
    }
}

int map_manager_get_zoom(void)
{
    if (!s_map_view) return HZ_DEFAULT_ZOOM;
    return map_view_get_zoom(s_map_view);
}

static void zoom_in_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        map_manager_zoom_in();
    }
}

static void zoom_out_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        map_manager_zoom_out();
    }
}

static void center_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        map_manager_center_on_gps();
    }
}

bool map_manager_init(lv_obj_t *parent_screen)
{
    if (parent_screen) {
        lv_obj_set_style_bg_color(parent_screen, lv_color_hex(0x0F172A), 0);
        lv_obj_set_style_bg_opa(parent_screen, LV_OPA_COVER, 0);
    }

    // 1. Create continuous map widget
    s_map_view = map_view_create(parent_screen);
    if (!s_map_view) return false;

    lv_obj_set_size(s_map_view, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_center(s_map_view);

    // 2. Attach tile cache (16 slots in PSRAM for RootMaker 2MB budget)
    map_view_set_tile_source(s_map_view, "/sdcard", "amap_cache", pick_cache_capacity(), true);

    map_view_style_t style;
    map_view_get_style(s_map_view, &style);
    style.route_color        = lv_color_hex(0x02CAFE);
    style.route_casing_color = lv_color_hex(0x047CBB);
    style.route_done_color   = lv_color_hex(0x94A3B8);
    style.route_width        = 20;   /* thick, like car / watch navigation */
    style.casing_width       = 28;
    style.marker_radius      = 10;
    map_view_set_style(s_map_view, &style);

    map_view_set_zoom_range(s_map_view, MIN_ZOOM, MAX_ZOOM);
    map_view_set_zoom(s_map_view, HZ_DEFAULT_ZOOM);
    map_view_set_center(s_map_view, HZ_DEFAULT_LAT, HZ_DEFAULT_LON);
    map_view_set_follow_offset(s_map_view, 40);
    s_rot_timer = lv_timer_create(rot_timer_cb, ROT_PERIOD_MS, NULL);

    lv_obj_t *top_layer = lv_layer_top();

    // Compact Navigation Status Card (240x240 layout, increased by 10%)
    s_status_card = lv_obj_create(top_layer);
    lv_obj_set_size(s_status_card, 234, 62);
    lv_obj_align(s_status_card, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_set_style_bg_color(s_status_card, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_status_card, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_status_card, 16, 0);
    lv_obj_set_style_border_width(s_status_card, 1, 0);
    lv_obj_set_style_border_color(s_status_card, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_pad_all(s_status_card, 4, 0);
    lv_obj_clear_flag(s_status_card, LV_OBJ_FLAG_SCROLLABLE);

    int icon_top = 4;
    measure_text_block(&icon_top, &s_icon_px);
    const int text_x = s_icon_px + 6;                       /* the text starts right after the arrow */
    const int row_w = 224 - text_x;

    s_turn_canvas_draw_buf = lv_draw_buf_create(ICON_PX, ICON_PX, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    s_turn_icon_canvas = lv_canvas_create(s_status_card);
    lv_canvas_set_draw_buf(s_turn_icon_canvas, s_turn_canvas_draw_buf);
    lv_obj_align(s_turn_icon_canvas, LV_ALIGN_TOP_LEFT, 0, icon_top);
    render_turn_icon(PHONE_GUIDE_STRAIGHT, 0);

    // Top Row: Distance & Maneuver Action (e.g. 79 (white) 米后 (gray) 右转 (white))
    s_top_row = lv_obj_create(s_status_card);
    lv_obj_set_size(s_top_row, row_w, ROW1_H);
    lv_obj_align(s_top_row, LV_ALIGN_TOP_LEFT, text_x, ROW1_Y);
    lv_obj_set_flex_flow(s_top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_top_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_top_row, 0, 0);
    lv_obj_set_style_pad_column(s_top_row, 4, 0);
    lv_obj_set_style_bg_opa(s_top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_top_row, 0, 0);
    lv_obj_clear_flag(s_top_row, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_dist_val = lv_label_create(s_top_row);
    lv_label_set_text(s_lbl_dist_val, "高德");
    lv_obj_set_style_text_color(s_lbl_dist_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_dist_val, &font_chinese_16, 0);

    s_lbl_dist_unit = lv_label_create(s_top_row);
    lv_label_set_text(s_lbl_dist_unit, "骑行");
    lv_obj_set_style_text_color(s_lbl_dist_unit, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(s_lbl_dist_unit, &font_chinese_16, 0);

    s_lbl_dist_action = lv_label_create(s_top_row);
    lv_label_set_text(s_lbl_dist_action, "导航");
    lv_obj_set_style_text_color(s_lbl_dist_action, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_dist_action, &font_chinese_16, 0);

    // Bottom Row: Target Road (e.g. 进入 (gray) 筑塘路 (white))
    s_bot_row = lv_obj_create(s_status_card);
    lv_obj_set_size(s_bot_row, row_w, ROW2_H);
    lv_obj_align(s_bot_row, LV_ALIGN_TOP_LEFT, text_x, ROW2_Y); // 4px padding between top and bottom!
    lv_obj_set_flex_flow(s_bot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bot_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_bot_row, 0, 0);
    lv_obj_set_style_pad_column(s_bot_row, 4, 0);
    lv_obj_set_style_bg_opa(s_bot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bot_row, 0, 0);
    lv_obj_clear_flag(s_bot_row, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_road_prefix = lv_label_create(s_bot_row);
    lv_label_set_text(s_lbl_road_prefix, "等待");
    lv_obj_set_style_text_color(s_lbl_road_prefix, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(s_lbl_road_prefix, &font_chinese_16, 0);

    s_lbl_road_name = lv_label_create(s_bot_row);
    lv_label_set_text(s_lbl_road_name, "手机选择目的地");
    lv_obj_set_style_text_color(s_lbl_road_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_road_name, &font_chinese_16, 0);
    lv_label_set_long_mode(s_lbl_road_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_road_name, row_w - 44);

    // ── Floating Zoom Pill on Right Edge (+ / Z16 / -) ──
    s_zoom_bar = lv_obj_create(top_layer);
    lv_obj_set_size(s_zoom_bar, 34, 86);
    lv_obj_align(s_zoom_bar, LV_ALIGN_RIGHT_MID, -4, 14);
    lv_obj_set_style_bg_color(s_zoom_bar, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_zoom_bar, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_zoom_bar, 17, 0);
    lv_obj_set_style_border_width(s_zoom_bar, 1, 0);
    lv_obj_set_style_border_color(s_zoom_bar, lv_color_hex(0x334155), 0);
    lv_obj_set_style_pad_all(s_zoom_bar, 2, 0);
    lv_obj_clear_flag(s_zoom_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Zoom IN button (+)
    s_zoom_in_btn = lv_button_create(s_zoom_bar);
    lv_obj_set_size(s_zoom_in_btn, 28, 26);
    lv_obj_align(s_zoom_in_btn, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(s_zoom_in_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_zoom_in_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_zoom_in_btn, zoom_in_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *in_lbl = lv_label_create(s_zoom_in_btn);
    lv_label_set_text(in_lbl, LV_SYMBOL_PLUS);
    lv_obj_center(in_lbl);
    lv_obj_set_style_text_color(in_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(in_lbl, &lv_font_montserrat_14, 0);

    // Zoom level text in middle
    s_zoom_lbl = lv_label_create(s_zoom_bar);
    lv_obj_align(s_zoom_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_zoom_lbl, "Z16");
    lv_obj_set_style_text_font(s_zoom_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_zoom_lbl, lv_color_hex(0xF8FAFC), 0);

    // Zoom OUT button (-)
    s_zoom_out_btn = lv_button_create(s_zoom_bar);
    lv_obj_set_size(s_zoom_out_btn, 28, 26);
    lv_obj_align(s_zoom_out_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_zoom_out_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_zoom_out_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_zoom_out_btn, zoom_out_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *out_lbl = lv_label_create(s_zoom_out_btn);
    lv_label_set_text(out_lbl, LV_SYMBOL_MINUS);
    lv_obj_center(out_lbl);
    lv_obj_set_style_text_color(out_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(out_lbl, &lv_font_montserrat_14, 0);

    // Floating Re-center Button (Bottom Right)
    s_center_btn = lv_button_create(top_layer);
    lv_obj_set_size(s_center_btn, 38, 38);
    lv_obj_align(s_center_btn, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_set_style_radius(s_center_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_center_btn, lv_color_hex(0x0284c7), 0);
    lv_obj_add_event_cb(s_center_btn, center_btn_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *btn_lbl = lv_label_create(s_center_btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_GPS);
    lv_obj_center(btn_lbl);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xffffff), 0);

    // ── Speed badge (bottom-left) ──
    s_speed_badge = lv_obj_create(top_layer);
    lv_obj_set_size(s_speed_badge, 40, 40);
    lv_obj_align(s_speed_badge, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_set_style_bg_color(s_speed_badge, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_speed_badge, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_speed_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_speed_badge, 2, 0);
    lv_obj_set_style_border_color(s_speed_badge, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_pad_all(s_speed_badge, 0, 0);
    lv_obj_clear_flag(s_speed_badge, LV_OBJ_FLAG_SCROLLABLE);
    s_lbl_speed = lv_label_create(s_speed_badge);
    lv_label_set_text(s_lbl_speed, "0");
    lv_obj_set_style_text_font(s_lbl_speed, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_speed, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_lbl_speed, LV_ALIGN_CENTER, 0, -5);
    lv_obj_t *spd_unit = lv_label_create(s_speed_badge);
    lv_label_set_text(spd_unit, "km/h");
    lv_obj_set_style_text_font(spd_unit, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(spd_unit, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(spd_unit, LV_ALIGN_CENTER, 0, 9);

    show_idle();
    update_zoom_ui();

    return true;
}

