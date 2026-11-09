#include "map_view.h"
#include "map_geo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "map_view";

#define REFRESH_PERIOD_MS       33      /* 30 FPS fluid refresh rate */
#define FOLLOW_EASE             0.35    /* fraction of the remaining gap per tick */
#define MOMENTUM_DECAY          0.92
#define MOMENTUM_STOP_PX        0.5
#define DRAG_START_PX           2       /* immediate responsive touch tracking */
#define ROUTE_MAX_SEGMENTS      192     /* frame-budget guard */
#define POS_EASE                0.30    /* marker glides toward each new fix */
#define POS_SNAP_DEG            1e-7    /* ~1 cm: stop easing */
#define POS_JUMP_DEG            0.003   /* ~300 m: teleport instead of gliding */
#define SNAP_ENTER_M            20.0    /* within this of the route: lock marker to it */
#define SNAP_EXIT_M             28.0    /* ...and only release past this (hysteresis) */
#define PROGRESS_TOL_M          35.0    /* map-matching tolerance for progress */
#define FOLLOW_LOCK_PX          24.0    /* closer than this the camera locks on, no easing */
#define TILE_PX                 MAP_TILE_CACHE_TILE_SIZE
#define MV_EDGE_ARROW_INSET     26      /* keeps the arrow off a round bezel */

extern const lv_font_t font_chinese_16;

typedef struct {
    lv_obj_t               *obj;
    map_tile_cache_handle_t cache;
    map_route_handle_t      route;

    int    zoom;
    int    min_zoom;
    int    max_zoom;
    double center_wx;       /* world pixels at `zoom` */
    double center_wy;

    bool   pos_valid;
    double pos_lat;
    double pos_lon;
    double pos_heading;
    bool   off_route;
    bool   return_active;
    double return_lat;
    double return_lon;
    uint32_t traveled_m;

    /* Marker as drawn: eased, and locked onto the route when within SNAP range. */
    double dpos_lat;
    double dpos_lon;
    double tgt_lat;
    double tgt_lon;
    bool   snapped;

    /* Progress along the route (map-matched). prog_seg < 0 = nothing driven yet. */
    int    prog_seg;
    double prog_t;

    /* Route projected to world pixels at pj_zoom, relative to the first point. */
    float *pj_x;
    float *pj_y;
    uint8_t *pj_minz;
    int    pj_cap;
    int    pj_count;
    int    pj_zoom;
    double pj_ox;
    double pj_oy;
    map_route_handle_t pj_route;

    int    tl_count;
    double tl_lats[32];
    double tl_lons[32];
    double tl_wx[32];
    double tl_wy[32];
    int    tl_zoom;

    bool    follow;
    int32_t follow_offset;
    double  rotation_deg;
    double  rotation_rad;

    bool    pressing;
    bool    drag_active;
    int32_t press_travel;
    double  vel_x;
    double  vel_y;

    map_view_style_t style;
    bool             debug;
    bool             warned_segment_cap;

    lv_timer_t *timer;
    uint32_t    cache_version;
    uint32_t    route_prefetch_tick;
} map_view_t;

/* --------------------------------------------------------------- helpers */

static map_view_t *mv_of(lv_obj_t *obj)
{
    return obj ? (map_view_t *)lv_obj_get_user_data(obj) : NULL;
}

static void mv_default_style(map_view_style_t *s)
{
    s->route_color        = lv_color_hex(0x02CAFE); /* Exact AMap electric cyan */
    s->route_done_color   = lv_color_hex(0x94A3B8); /* Traveled path behind vehicle */
    s->route_casing_color = lv_color_hex(0x047CBB); /* Exact AMap cobalt blue border */
    s->marker_color       = lv_color_hex(0x1A73E8);
    s->marker_off_color   = lv_color_hex(0xEF4444);
    s->return_color       = lv_color_hex(0xFF8A3D);
    s->destination_color  = lv_color_hex(0xEF4444); /* Red finish circle */
    s->route_width        = 16;
    s->casing_width       = 24;
    s->marker_radius      = 13;
}

/** Recompute the world-pixel centre so the same place stays centred at a new zoom. */
static void mv_reproject_center(map_view_t *mv, int new_zoom)
{
    double lat = map_geo_world_py_to_lat(mv->center_wy, mv->zoom);
    double lon = map_geo_world_px_to_lon(mv->center_wx, mv->zoom);
    mv->zoom = new_zoom;
    mv->center_wx = map_geo_lon_to_world_px(lon, new_zoom);
    mv->center_wy = map_geo_lat_to_world_py(lat, new_zoom);
}

/** Clamp vertically to the world and wrap horizontally. */
static void mv_clamp_center(map_view_t *mv)
{
    double world = map_geo_world_size_px(mv->zoom);
    if (mv->center_wx < 0)      mv->center_wx += world;
    if (mv->center_wx >= world) mv->center_wx -= world;
    if (mv->center_wy < 0)      mv->center_wy = 0;
    if (mv->center_wy > world)  mv->center_wy = world;
}

/** Where the follow target wants the centre, given the on-screen marker offset. */
static bool mv_follow_target(map_view_t *mv, double *wx, double *wy)
{
    if (!mv->pos_valid) return false;
    *wx = map_geo_lon_to_world_px(mv->dpos_lon, mv->zoom);
    *wy = map_geo_lat_to_world_py(mv->dpos_lat, mv->zoom);
    return true;
}

static void mv_content_area(map_view_t *mv, lv_area_t *out)
{
    lv_obj_get_content_coords(mv->obj, out);
}

/* ------------------------------------------------------------- rendering */

typedef struct {
    lv_area_t content;
    double    origin_wx;    /* world pixel at content.x1 */
    double    origin_wy;
    double    center_wx;
    double    center_wy;
    double    cx;
    double    cy;
    double    anchor_offset_y;
    double    rotation_deg;
    double    rotation_rad;
    double    cos_r;
    double    sin_r;
    bool      has_rotation;
} render_ctx_t;

static void mv_make_ctx(map_view_t *mv, render_ctx_t *ctx)
{
    mv_content_area(mv, &ctx->content);
    int32_t w = lv_area_get_width(&ctx->content);
    int32_t h = lv_area_get_height(&ctx->content);
    ctx->origin_wx = mv->center_wx - w / 2.0;
    ctx->origin_wy = mv->center_wy - h / 2.0;
    ctx->center_wx = mv->center_wx;
    ctx->center_wy = mv->center_wy;
    ctx->cx = (ctx->content.x1 + ctx->content.x2) / 2.0;
    ctx->cy = (ctx->content.y1 + ctx->content.y2) / 2.0;
    ctx->anchor_offset_y = (double)mv->follow_offset;
    ctx->rotation_deg = mv->rotation_deg;
    ctx->rotation_rad = mv->rotation_rad;
    ctx->has_rotation = (fabs(mv->rotation_deg) > 0.05);
    if (ctx->has_rotation) {
        ctx->cos_r = cos(ctx->rotation_rad);
        ctx->sin_r = sin(ctx->rotation_rad);
    } else {
        ctx->cos_r = 1.0;
        ctx->sin_r = 0.0;
    }
}

static inline void mv_world_to_screen(const render_ctx_t *ctx, double wx, double wy,
                                      double *sx, double *sy)
{
    if (!ctx->has_rotation) {
        *sx = ctx->content.x1 + (wx - ctx->origin_wx);
        *sy = ctx->content.y1 + (wy - ctx->origin_wy) + ctx->anchor_offset_y;
        return;
    }

    double dx = wx - ctx->center_wx;
    double dy = wy - ctx->center_wy;

    *sx = ctx->cx + (dx * ctx->cos_r - dy * ctx->sin_r);
    *sy = (ctx->cy + ctx->anchor_offset_y) + (dx * ctx->sin_r + dy * ctx->cos_r);
}

static void mv_draw_tiles(map_view_t *mv, lv_layer_t *layer, const render_ctx_t *ctx)
{
    int32_t w = lv_area_get_width(&ctx->content);
    int32_t h = lv_area_get_height(&ctx->content);

    int fetch_zoom = mv->zoom;
    int scale_mult = 1;
    int tile_display_size = TILE_PX;

    /* Virtual Over-Zoom: AMap stops at z18. For z19 (2x) and z20 (5x), scale parent z18 tiles */
    if (mv->zoom > 18) {
        fetch_zoom = 18;
        if (mv->zoom == 19) {
            scale_mult = 2;
        } else if (mv->zoom == 20) {
            scale_mult = 5;          /* 5x digital over-zoom (upgraded from 4x) */
        } else {
            scale_mult = 8;          /* 8x for z21 */
        }
        tile_display_size = TILE_PX * scale_mult;   /* 512 for z19, 1280 for z20 */
    }

    int n = 1 << fetch_zoom;

    int tx0, ty0, tx1, ty1;
    if (!ctx->has_rotation) {
        tx0 = (int)floor(ctx->origin_wx / (double)tile_display_size);
        ty0 = (int)floor(ctx->origin_wy / (double)tile_display_size);
        tx1 = (int)floor((ctx->origin_wx + w - 1) / (double)tile_display_size);
        ty1 = (int)floor((ctx->origin_wy + h - 1) / (double)tile_display_size);
    } else {
        /* Bounding radius around center: sqrt(w^2 + h^2) / 2 */
        double radius = sqrt(w * w + h * h) / 2.0;
        tx0 = (int)floor((ctx->center_wx - radius) / (double)tile_display_size);
        ty0 = (int)floor((ctx->center_wy - radius) / (double)tile_display_size);
        tx1 = (int)floor((ctx->center_wx + radius) / (double)tile_display_size);
        ty1 = (int)floor((ctx->center_wy + radius) / (double)tile_display_size);
    }

    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.antialias = 0;
    if (scale_mult > 1) {
        img_dsc.scale_x = LV_SCALE_NONE * scale_mult;
        img_dsc.scale_y = LV_SCALE_NONE * scale_mult;
        img_dsc.pivot.x = 0;
        img_dsc.pivot.y = 0;
    }
    if (ctx->has_rotation) {
        img_dsc.rotation = (int32_t)lround(ctx->rotation_deg * 10.0);
        img_dsc.pivot.x = TILE_PX / 2;
        img_dsc.pivot.y = TILE_PX / 2;
    }

    lv_draw_rect_dsc_t hole_dsc;
    lv_draw_rect_dsc_init(&hole_dsc);
    hole_dsc.bg_color = lv_color_hex(0xE8E4DD);   /* neutral "paper" while loading */
    hole_dsc.bg_opa   = LV_OPA_COVER;

    int visible = (tx1 - tx0 + 1) * (ty1 - ty0 + 1);

    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            lv_area_t img_area;
            lv_area_t hole_area;
            if (!ctx->has_rotation) {
                int32_t sx = ctx->content.x1 + (int32_t)lround(tx * (double)tile_display_size - ctx->origin_wx);
                int32_t sy = ctx->content.y1 + (int32_t)lround(ty * (double)tile_display_size - ctx->origin_wy) + (int32_t)lround(ctx->anchor_offset_y);
                img_area.x1 = sx;
                img_area.y1 = sy;
                img_area.x2 = sx + TILE_PX - 1;
                img_area.y2 = sy + TILE_PX - 1;

                hole_area.x1 = sx;
                hole_area.y1 = sy;
                hole_area.x2 = sx + tile_display_size - 1;
                hole_area.y2 = sy + tile_display_size - 1;
            } else {
                double tc_wx = tx * (double)tile_display_size + tile_display_size / 2.0;
                double tc_wy = ty * (double)tile_display_size + tile_display_size / 2.0;
                double scx, scy;
                mv_world_to_screen(ctx, tc_wx, tc_wy, &scx, &scy);

                int32_t half_unscaled = TILE_PX / 2;
                img_area.x1 = (int32_t)lround(scx - half_unscaled);
                img_area.y1 = (int32_t)lround(scy - half_unscaled);
                img_area.x2 = img_area.x1 + TILE_PX - 1;
                img_area.y2 = img_area.y1 + TILE_PX - 1;

                int32_t half_disp = tile_display_size / 2;
                hole_area.x1 = (int32_t)lround(scx - half_disp);
                hole_area.y1 = (int32_t)lround(scy - half_disp);
                hole_area.x2 = hole_area.x1 + tile_display_size - 1;
                hole_area.y2 = hole_area.y1 + tile_display_size - 1;
            }

            /* Above or below the world: nothing exists there. */
            if (ty < 0 || ty >= n) {
                lv_draw_rect(layer, &hole_dsc, &hole_area);
                continue;
            }

            int wrapped_tx = ((tx % n) + n) % n;
            lv_image_dsc_t *img = map_tile_cache_get(mv->cache, fetch_zoom, wrapped_tx, ty, true);
            if (img) {
                img_dsc.src = img;
                lv_draw_image(layer, &img_dsc, &img_area);
            } else {
                lv_draw_rect(layer, &hole_dsc, &hole_area);
            }
        }
    }

    /* Warm tiles just outside the viewport so a pan finds them already there.
     *  - while the map is moving, only the side it is moving toward (everything
     *    else would just compete with the tiles that are on screen);
     *  - while riding a route, skip the blind ring: mv_prefetch_route() loads the
     *    road ahead, which is what will actually be needed. */
    const bool moving = (fabs(mv->vel_x) > 0.5 || fabs(mv->vel_y) > 0.5);
    const bool riding = (mv->route != NULL && mv->prog_seg >= 0);
    int ring = (tx1 - tx0 + 3) * (ty1 - ty0 + 3) - visible;
    map_tile_cache_stats_t st;
    map_tile_cache_get_stats(mv->cache, &st);
    if ((moving || !riding) && st.capacity >= visible + (moving ? 6 : ring)) {
        for (int ty = ty0 - 1; ty <= ty1 + 1; ty++) {
            if (ty < 0 || ty >= n) continue;
            for (int tx = tx0 - 1; tx <= tx1 + 1; tx++) {
                if (tx >= tx0 && tx <= tx1 && ty >= ty0 && ty <= ty1) continue;
                if (moving) {
                    bool ahead = (tx < tx0 && mv->vel_x < -0.5) || (tx > tx1 && mv->vel_x > 0.5) ||
                                 (ty < ty0 && mv->vel_y < -0.5) || (ty > ty1 && mv->vel_y > 0.5);
                    if (!ahead) continue;
                }
                map_tile_cache_prefetch(mv->cache, fetch_zoom, ((tx % n) + n) % n, ty);
            }
        }
    }
}

/** Cheap reject: does the segment's bounding box touch the drawing area? */
static bool mv_segment_visible(const lv_area_t *content, double x1, double y1,
                               double x2, double y2, int32_t margin)
{
    double lo_x = x1 < x2 ? x1 : x2;
    double hi_x = x1 < x2 ? x2 : x1;
    double lo_y = y1 < y2 ? y1 : y2;
    double hi_y = y1 < y2 ? y2 : y1;

    if (hi_x < content->x1 - margin) return false;
    if (lo_x > content->x2 + margin) return false;
    if (hi_y < content->y1 - margin) return false;
    if (lo_y > content->y2 + margin) return false;
    return true;
}

/* ------------------------------------------------------- sprites (AA art) */

/*
 * The position arrow and the route chevrons are rendered ONCE into small
 * anti-aliased ARGB8888 sprites and then drawn with lv_draw_image(), letting
 * LVGL rotate them with filtering.
 *
 * This replaces drawing them as rotated lv_draw_triangle() pairs, which were
 * rounded to integer pixels per vertex: two triangles sharing an edge would
 * sometimes rasterise one wing short or not at all, and edges were jagged.
 */

#define ARROW_SPRITE_PX   80
#define ARROW_SCALE       1.25f   /* big, car-navigation style position arrow */
#define CHEVRON_SPRITE_PX 28
#define CHEVRON_SCALE     1.45f

typedef struct {
    const float *xy;        /* x0,y0,x1,y1,... relative to the sprite centre */
    int          n;
    uint32_t     rgb;
} mv_poly_t;

static lv_image_dsc_t s_arrow_on_dsc;
static lv_image_dsc_t s_arrow_off_dsc;
static lv_image_dsc_t s_chevron_dsc;
static bool           s_arrow_on_ok, s_arrow_off_ok, s_chevron_ok;

static void *mv_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(bytes);
    return p;
}

static bool mv_pt_in_poly(const float *xy, int n, float x, float y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = xy[2 * i], yi = xy[2 * i + 1];
        float xj = xy[2 * j], yj = xy[2 * j + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) in = !in;
    }
    return in;
}

/** 4x4 supersampled raster of @p np polygons (later = on top) into straight-alpha ARGB8888. */
static bool mv_sprite_build(lv_image_dsc_t *dsc, int size, const mv_poly_t *polys, int np)
{
    uint8_t *buf = (uint8_t *)mv_alloc((size_t)size * size * 4);
    if (!buf) return false;

    const int SS = 4;
    const float half = size / 2.0f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int cnt = 0;
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    float fx = x + (sx + 0.5f) / SS - half;
                    float fy = y + (sy + 0.5f) / SS - half;
                    for (int p = np - 1; p >= 0; p--) {
                        if (mv_pt_in_poly(polys[p].xy, polys[p].n, fx, fy)) {
                            r += (polys[p].rgb >> 16) & 0xFF;
                            g += (polys[p].rgb >> 8) & 0xFF;
                            b += polys[p].rgb & 0xFF;
                            cnt++;
                            break;
                        }
                    }
                }
            }
            uint8_t *px = buf + ((size_t)y * size + x) * 4;
            if (cnt == 0) {
                px[0] = px[1] = px[2] = px[3] = 0;
            } else {
                px[0] = (uint8_t)(b / cnt);
                px[1] = (uint8_t)(g / cnt);
                px[2] = (uint8_t)(r / cnt);
                px[3] = (uint8_t)((cnt * 255) / (SS * SS));
            }
        }
    }

    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w      = size;
    dsc->header.h      = size;
    dsc->header.stride = size * 4;
    dsc->data          = buf;
    dsc->data_size     = (uint32_t)size * size * 4;
    return true;
}

static const lv_image_dsc_t *mv_arrow_sprite(bool off_route)
{
    /* Heading is "up" (-y). Outer white silhouette, then two shaded wings. */
    static const float outer[] = { 0.0f, -23.5f,  -14.0f, 12.0f,  0.0f, 5.0f,  14.0f, 12.0f };
    static const float left[]  = { 0.0f, -21.0f,  -12.0f, 10.5f,  0.0f, 4.0f };
    static const float right[] = { 0.0f, -21.0f,   0.0f, 4.0f,   12.0f, 10.5f };

    bool *ok = off_route ? &s_arrow_off_ok : &s_arrow_on_ok;
    lv_image_dsc_t *dsc = off_route ? &s_arrow_off_dsc : &s_arrow_on_dsc;
    if (!*ok) {
        float so[8], sl[6], sr[6];
        for (int i = 0; i < 8; i++) so[i] = outer[i] * ARROW_SCALE;
        for (int i = 0; i < 6; i++) { sl[i] = left[i] * ARROW_SCALE; sr[i] = right[i] * ARROW_SCALE; }
        mv_poly_t polys[3] = {
            { so, 4, 0xFFFFFF },
            { sl, 3, off_route ? 0xEF4444 : 0x0091FF },
            { sr, 3, off_route ? 0xDC2626 : 0x0066CC },
        };
        *ok = mv_sprite_build(dsc, ARROW_SPRITE_PX, polys, 3);
    }
    return *ok ? dsc : NULL;
}

static const lv_image_dsc_t *mv_chevron_sprite(void)
{
    /* A bold ">" pointing up, optically centred on the sprite centre. */
    static const float shape[] = {
         0.0f, -4.1f,
         5.8f,  1.7f,
         3.4f,  4.1f,
         0.0f,  0.71f,
        -3.4f,  4.1f,
        -5.8f,  1.7f,
    };
    if (!s_chevron_ok) {
        float sh[12];
        for (int i = 0; i < 12; i++) sh[i] = shape[i] * CHEVRON_SCALE;
        mv_poly_t polys[1] = { { sh, 6, 0xFFFFFF } };
        s_chevron_ok = mv_sprite_build(&s_chevron_dsc, CHEVRON_SPRITE_PX, polys, 1);
    }
    return s_chevron_ok ? &s_chevron_dsc : NULL;
}

/* ------------------------------------------------- cached route projection */

/*
 * Projecting a lat/lon to world pixels costs a log() and a tan() in DOUBLE
 * precision, which the ESP32-S3 emulates in software. Doing that for every
 * route point on every frame is what made dragging stutter once a route was
 * on screen. The projection only changes with zoom, so do it once per
 * (route, zoom) and keep it as floats relative to the first point.
 */

static void mv_pj_free(map_view_t *mv)
{
    free(mv->pj_x);
    free(mv->pj_y);
    free(mv->pj_minz);
    mv->pj_x = mv->pj_y = NULL;
    mv->pj_minz = NULL;
    mv->pj_cap = mv->pj_count = 0;
    mv->pj_route = NULL;
}

static bool mv_pj_ensure(map_view_t *mv)
{
    if (!mv->route) return false;
    if (mv->pj_x && mv->pj_route == mv->route && mv->pj_zoom == mv->zoom) return true;

    int n = map_route_get_point_count(mv->route);
    if (n < 2) return false;

    if (!mv->pj_x || n > mv->pj_cap) {
        mv_pj_free(mv);
        mv->pj_x    = (float *)mv_alloc((size_t)n * sizeof(float));
        mv->pj_y    = (float *)mv_alloc((size_t)n * sizeof(float));
        mv->pj_minz = (uint8_t *)mv_alloc((size_t)n);
        if (!mv->pj_x || !mv->pj_y || !mv->pj_minz) {
            mv_pj_free(mv);
            return false;
        }
        mv->pj_cap = n;
    }

    for (int i = 0; i < n; i++) {
        map_route_point_t p;
        if (!map_route_get_point(mv->route, i, &p)) { n = i; break; }
        double wx = map_geo_lon_to_world_px(p.lon, mv->zoom);
        double wy = map_geo_lat_to_world_py(p.lat, mv->zoom);
        if (i == 0) {
            mv->pj_ox = wx;
            mv->pj_oy = wy;
        }
        mv->pj_x[i]    = (float)(wx - mv->pj_ox);
        mv->pj_y[i]    = (float)(wy - mv->pj_oy);
        mv->pj_minz[i] = p.min_zoom;
    }

    mv->pj_count = n;
    mv->pj_route = mv->route;
    mv->pj_zoom  = mv->zoom;
    return n >= 2;
}

/* ---------------------------------------------------------------- route */

#define ROUTE_TOL_PX        0.6f    /* max distance a dropped vertex may sit off the drawn line */
#define RUN_MAX             48
#define MAX_VIS_SEGS        ROUTE_MAX_SEGMENTS
#define MAX_CHEVRONS        40
#define CHEVRON_SPACING_PX  44.0f
#define CHEVRON_PHASE_PX    22.0f
#define SHARP_JOINT_COS     0.9962f /* ~5 deg: above this a flat butt joint is invisible */

typedef struct {
    float   x1, y1, x2, y2;
    uint8_t done;
    uint8_t cap1, cap2;
} vis_segment_t;

typedef struct {
    float   x, y;
    int32_t rot;                    /* 0.1 degree, 0 = pointing up */
} vis_chevron_t;

/* Draw runs under the LVGL lock on a single task, so static buffers are safe
 * and keep ~5 KB off the (small) LVGL task stack. */
static vis_segment_t s_segs[MAX_VIS_SEGS];
static vis_chevron_t s_chev[MAX_CHEVRONS];

typedef struct {
    float lx1, ly1, lx2, ly2;       /* cull rect (content + margin) */
    int   nseg;
    bool  capped;

    bool  have_anchor;
    float ax, ay;                   /* start of the run being merged */
    float lastx, lasty;             /* previous fed point */
    bool  run_done;
    int   run_n;
    float rx[RUN_MAX], ry[RUN_MAX]; /* points after the anchor; last one is the run's end */
} route_pass_t;

static void rp_emit(route_pass_t *rp, float x1, float y1, float x2, float y2, bool done)
{
    if ((x1 < rp->lx1 && x2 < rp->lx1) || (x1 > rp->lx2 && x2 > rp->lx2) ||
        (y1 < rp->ly1 && y2 < rp->ly1) || (y1 > rp->ly2 && y2 > rp->ly2)) {
        return;
    }
    if (rp->nseg >= MAX_VIS_SEGS) {
        rp->capped = true;
        return;
    }
    vis_segment_t *s = &s_segs[rp->nseg++];
    s->x1 = x1; s->y1 = y1; s->x2 = x2; s->y2 = y2;
    s->done = done ? 1 : 0;
    s->cap1 = s->cap2 = 0;
}

static void rp_flush(route_pass_t *rp)
{
    if (rp->run_n <= 0) return;
    float ex = rp->rx[rp->run_n - 1];
    float ey = rp->ry[rp->run_n - 1];
    rp_emit(rp, rp->ax, rp->ay, ex, ey, rp->run_done);
    rp->ax = ex;
    rp->ay = ey;
    rp->run_n = 0;
}

/**
 * Feed the next screen-space vertex. Collinear vertices are merged into one
 * segment, but ONLY while every dropped vertex stays within ROUTE_TOL_PX of
 * the merged line - so curves keep their shape and the line stays centred on
 * the road (the old "step >= 14 px" rule cut corners).
 */
static void rp_feed(route_pass_t *rp, float qx, float qy, bool done)
{
    if (!rp->have_anchor) {
        rp->have_anchor = true;
        rp->ax = rp->lastx = qx;
        rp->ay = rp->lasty = qy;
        rp->run_n = 0;
        return;
    }

    float px = rp->lastx, py = rp->lasty;
    rp->lastx = qx;
    rp->lasty = qy;

    /* Both ends beyond the same screen edge: nothing to draw, skip the work. */
    if ((px < rp->lx1 && qx < rp->lx1) || (px > rp->lx2 && qx > rp->lx2) ||
        (py < rp->ly1 && qy < rp->ly1) || (py > rp->ly2 && qy > rp->ly2)) {
        rp_flush(rp);
        rp->ax = qx;
        rp->ay = qy;
        rp->run_n = 0;
        return;
    }

    if (rp->run_n == 0) {
        rp->run_done = done;
    } else if (done != rp->run_done) {
        rp_flush(rp);
        rp->run_done = done;
    }

    bool ok = (rp->run_n < RUN_MAX);
    if (ok && rp->run_n > 0) {
        float dx = qx - rp->ax, dy = qy - rp->ay;
        float len2 = dx * dx + dy * dy;
        float tol2 = ROUTE_TOL_PX * ROUTE_TOL_PX;
        for (int j = 0; j < rp->run_n; j++) {
            float ex = rp->rx[j] - rp->ax, ey = rp->ry[j] - rp->ay;
            float d2;
            if (len2 < 1e-6f) {
                d2 = ex * ex + ey * ey;
            } else {
                float cr = ex * dy - ey * dx;
                d2 = (cr * cr) / len2;
            }
            if (d2 > tol2) { ok = false; break; }
        }
    }
    if (!ok) {
        rp_flush(rp);
        rp->run_done = done;
    }

    rp->rx[rp->run_n] = qx;
    rp->ry[rp->run_n] = qy;
    rp->run_n++;
}

/** Liang-Barsky: parametric range of P0->P1 inside the rect. */
static bool mv_clip_t(float x0, float y0, float x1, float y1,
                      float xl, float yl, float xh, float yh, float *t0, float *t1)
{
    float dx = x1 - x0, dy = y1 - y0;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { x0 - xl, xh - x0, y0 - yl, yh - y0 };
    float a = 0.0f, b = 1.0f;
    for (int i = 0; i < 4; i++) {
        if (fabsf(p[i]) < 1e-9f) {
            if (q[i] < 0.0f) return false;
        } else {
            float r = q[i] / p[i];
            if (p[i] < 0.0f) { if (r > b) return false; if (r > a) a = r; }
            else             { if (r < a) return false; if (r < b) b = r; }
        }
    }
    *t0 = a;
    *t1 = b;
    return true;
}

static bool mv_joint_sharp(const vis_segment_t *a, const vis_segment_t *b)
{
    float ax = a->x2 - a->x1, ay = a->y2 - a->y1;
    float bx = b->x2 - b->x1, by = b->y2 - b->y1;
    float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
    if (la < 0.01f || lb < 0.01f) return false;
    return ((ax * bx + ay * by) / (la * lb)) < SHARP_JOINT_COS;
}

static int mv_draw_route(map_view_t *mv, lv_layer_t *layer, const render_ctx_t *ctx)
{
    if (!mv->route || !mv_pj_ensure(mv)) return 0;

    int32_t casing_w = mv->style.casing_width;
    int32_t core_w   = mv->style.route_width;
    if (core_w <= 0 && casing_w <= 0) return 0;

    if (mv->zoom >= 20) {
        casing_w += 6;
        core_w   += 4;
    } else if (mv->zoom <= 14) {
        casing_w = casing_w / 2 + 2;
        core_w   = core_w / 2;
    }

    const int   n      = mv->pj_count;
    const float margin = (float)((casing_w > core_w ? casing_w : core_w) + 4);

    route_pass_t rp;
    memset(&rp, 0, sizeof(rp));
    rp.lx1 = ctx->content.x1 - margin;
    rp.ly1 = ctx->content.y1 - margin;
    rp.lx2 = ctx->content.x2 + margin;
    rp.ly2 = ctx->content.y2 + margin;

    /* world(relative to route origin) -> screen, in float */
    const float rcx = (float)(mv->center_wx - mv->pj_ox);
    const float rcy = (float)(mv->center_wy - mv->pj_oy);
    const float cs  = (float)ctx->cos_r;
    const float sn  = (float)ctx->sin_r;
    const float w2  = lv_area_get_width(&ctx->content) / 2.0f;
    const float h2  = lv_area_get_height(&ctx->content) / 2.0f;
    const float bx  = ctx->has_rotation ? (float)ctx->cx : ctx->content.x1 + w2;
    const float by  = (ctx->has_rotation ? (float)ctx->cy : ctx->content.y1 + h2)
                      + (float)ctx->anchor_offset_y;

    /* Where the rider is on the route: everything before is "done". */
    const int   prog_seg = (mv->prog_seg >= 0 && mv->prog_seg < n - 1) ? mv->prog_seg : -1;
    const float prog_t   = (float)mv->prog_t;
    bool        split_pending = (prog_seg >= 0);
    float       vx = 0.0f, vy = 0.0f;       /* progress point, screen space */
    if (prog_seg >= 0) {
        float wx = mv->pj_x[prog_seg] + prog_t * (mv->pj_x[prog_seg + 1] - mv->pj_x[prog_seg]);
        float wy = mv->pj_y[prog_seg] + prog_t * (mv->pj_y[prog_seg + 1] - mv->pj_y[prog_seg]);
        float dx = wx - rcx, dy = wy - rcy;
        vx = bx + dx * cs - dy * sn;
        vy = by + dx * sn + dy * cs;
    }

    const bool want_chev = (mv->zoom >= 15);
    int   nchev = 0;
    float s_acc = 0.0f;                    /* arclength (world px) along the original polyline */
    int   prev_i = -1;
    float prev_sx = 0, prev_sy = 0, prev_wx = 0, prev_wy = 0;

    for (int i = 0; i < n; i++) {
        if (mv->pj_minz[i] > mv->zoom) continue;

        float wx = mv->pj_x[i], wy = mv->pj_y[i];
        float dx = wx - rcx, dy = wy - rcy;
        float sx = bx + dx * cs - dy * sn;
        float sy = by + dx * sn + dy * cs;

        if (prev_i < 0) {
            rp_feed(&rp, sx, sy, false);
        } else {
            /* ---- chevrons: spaced along the real polyline, not the decimated one ---- */
            float sdx = wx - prev_wx, sdy = wy - prev_wy;
            float len = sqrtf(sdx * sdx + sdy * sdy);
            if (want_chev && nchev < MAX_CHEVRONS && len > 0.01f) {
                float t0, t1;
                if (mv_clip_t(prev_sx, prev_sy, sx, sy, rp.lx1, rp.ly1, rp.lx2, rp.ly2, &t0, &t1)) {
                    float a0 = s_acc + t0 * len, a1 = s_acc + t1 * len;
                    int   k  = (int)ceilf((a0 - CHEVRON_PHASE_PX) / CHEVRON_SPACING_PX);
                    float ang = 0.0f;
                    bool  have_ang = false;
                    for (;; k++) {
                        float s = CHEVRON_PHASE_PX + k * CHEVRON_SPACING_PX;
                        if (s > a1 || nchev >= MAX_CHEVRONS) break;
                        float t = (s - s_acc) / len;
                        if (prog_seg >= 0 && (prev_i < prog_seg || (prev_i == prog_seg && t < prog_t))) continue;
                        if (!have_ang) {
                            ang = atan2f(sx - prev_sx, -(sy - prev_sy)) * (1800.0f / (float)M_PI);
                            have_ang = true;
                        }
                        int32_t rot = (int32_t)lroundf(ang);
                        rot %= 3600;
                        if (rot < 0) rot += 3600;
                        s_chev[nchev].x = prev_sx + (sx - prev_sx) * t;
                        s_chev[nchev].y = prev_sy + (sy - prev_sy) * t;
                        s_chev[nchev].rot = rot;
                        nchev++;
                    }
                }
            }
            s_acc += len;

            /* ---- the line itself, split exactly at the rider's position ---- */
            if (split_pending && i > prog_seg) {
                rp_feed(&rp, vx, vy, true);     /* everything up to the rider: done */
                rp_feed(&rp, sx, sy, false);    /* rider -> next vertex: still ahead */
                split_pending = false;
            } else {
                rp_feed(&rp, sx, sy, prog_seg >= 0 && i <= prog_seg);
            }
        }

        prev_i = i;
        prev_sx = sx; prev_sy = sy;
        prev_wx = wx; prev_wy = wy;
    }
    rp_flush(&rp);

    const int seg_count = rp.nseg;
    if (seg_count == 0) return 0;

    /* Round the joints that bend (and the open ends); leave straight joints flat
     * so the join is seamless and we do not pay for a circle on every vertex. */
    for (int i = 0; i < seg_count; i++) {
        vis_segment_t *s = &s_segs[i];
        bool chained_prev = (i > 0) && s_segs[i - 1].x2 == s->x1 && s_segs[i - 1].y2 == s->y1;
        bool chained_next = (i < seg_count - 1) && s_segs[i + 1].x1 == s->x2 && s_segs[i + 1].y1 == s->y2;
        s->cap1 = (!chained_prev || mv_joint_sharp(&s_segs[i - 1], s)) ? 1 : 0;
        s->cap2 = (!chained_next || mv_joint_sharp(s, &s_segs[i + 1])) ? 1 : 0;
    }

    /* Two full passes (casing, then core) so a later segment's outline never
     * paints over an earlier segment's core at a joint. */
    for (int pass = 0; pass < 2; pass++) {
        int32_t w = (pass == 0) ? casing_w : core_w;
        if (w <= 0) continue;

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.width = w;
        dsc.opa   = LV_OPA_COVER;

        for (int i = 0; i < seg_count; i++) {
            const vis_segment_t *s = &s_segs[i];
            if (pass == 0) dsc.color = s->done ? lv_color_hex(0x64748B) : mv->style.route_casing_color;
            else           dsc.color = s->done ? mv->style.route_done_color : mv->style.route_color;
            dsc.round_start = s->cap1;
            dsc.round_end   = s->cap2;
            dsc.p1.x = (lv_value_precise_t)lroundf(s->x1);
            dsc.p1.y = (lv_value_precise_t)lroundf(s->y1);
            dsc.p2.x = (lv_value_precise_t)lroundf(s->x2);
            dsc.p2.y = (lv_value_precise_t)lroundf(s->y2);
            lv_draw_line(layer, &dsc);
        }
    }

    /* Direction chevrons on the road ahead (anti-aliased sprites). */
    const lv_image_dsc_t *chev = (nchev > 0) ? mv_chevron_sprite() : NULL;
    if (chev) {
        lv_draw_image_dsc_t img;
        lv_draw_image_dsc_init(&img);
        img.src       = chev;
        img.antialias = 1;
        img.pivot.x   = CHEVRON_SPRITE_PX / 2;
        img.pivot.y   = CHEVRON_SPRITE_PX / 2;
        if (mv->zoom >= 20) {
            img.scale_x = 307;
            img.scale_y = 307;
        }
        for (int i = 0; i < nchev; i++) {
            int32_t cx = (int32_t)lroundf(s_chev[i].x);
            int32_t cy = (int32_t)lroundf(s_chev[i].y);
            img.rotation = s_chev[i].rot;
            lv_area_t a = {
                .x1 = cx - CHEVRON_SPRITE_PX / 2,
                .y1 = cy - CHEVRON_SPRITE_PX / 2,
                .x2 = cx + CHEVRON_SPRITE_PX / 2 - 1,
                .y2 = cy + CHEVRON_SPRITE_PX / 2 - 1,
            };
            lv_draw_image(layer, &img, &a);
        }
    }

    return rp.capped ? MAX_VIS_SEGS : seg_count;
}

static void mv_draw_destination(map_view_t *mv, lv_layer_t *layer, const render_ctx_t *ctx)
{
    double lat, lon;
    if (!map_route_get_destination(mv->route, &lat, &lon)) return;

    double sx, sy;
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(lon, mv->zoom),
                       map_geo_lat_to_world_py(lat, mv->zoom),
                       &sx, &sy);

    int32_t r = mv->style.marker_radius;
    lv_area_t a = {
        .x1 = (int32_t)lround(sx) - r,
        .y1 = (int32_t)lround(sy) - r,
        .x2 = (int32_t)lround(sx) + r,
        .y2 = (int32_t)lround(sy) + r,
    };
    if (!mv_segment_visible(&ctx->content, a.x1, a.y1, a.x2, a.y2, 0)) return;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius       = LV_RADIUS_CIRCLE;
    dsc.bg_color     = mv->style.destination_color;
    dsc.bg_opa       = LV_OPA_COVER;
    dsc.border_color = lv_color_white();
    dsc.border_width = 3;
    dsc.border_opa   = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, &a);
}

static void mv_draw_origin(map_view_t *mv, lv_layer_t *layer, const render_ctx_t *ctx)
{
    double lat, lon;
    if (!map_route_get_start(mv->route, &lat, &lon)) return;

    double sx, sy;
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(lon, mv->zoom),
                       map_geo_lat_to_world_py(lat, mv->zoom),
                       &sx, &sy);

    int32_t r = mv->style.marker_radius - 2;
    lv_area_t a = {
        .x1 = (int32_t)lround(sx) - r,
        .y1 = (int32_t)lround(sy) - r,
        .x2 = (int32_t)lround(sx) + r,
        .y2 = (int32_t)lround(sy) + r,
    };
    if (!mv_segment_visible(&ctx->content, a.x1, a.y1, a.x2, a.y2, 0)) return;

    /* AMap green origin marker: green outer ring (#2EAF6C) with white border */
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius       = LV_RADIUS_CIRCLE;
    dsc.bg_color     = lv_color_hex(0x2EAF6C);
    dsc.bg_opa       = LV_OPA_COVER;
    dsc.border_color = lv_color_white();
    dsc.border_width = 3;
    dsc.border_opa   = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, &a);

    /* Inner white core dot */
    int32_t ir = 3;
    lv_area_t ia = {
        .x1 = (int32_t)lround(sx) - ir,
        .y1 = (int32_t)lround(sy) - ir,
        .x2 = (int32_t)lround(sx) + ir,
        .y2 = (int32_t)lround(sy) + ir,
    };
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius       = LV_RADIUS_CIRCLE;
    dsc.bg_color     = lv_color_white();
    dsc.bg_opa       = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, &ia);
}

static void mv_draw_traffic_lights(map_view_t *mv, lv_layer_t *layer, const render_ctx_t *ctx)
{
    if (mv->tl_count <= 0) return;

    if (mv->tl_zoom != mv->zoom) {
        for (int i = 0; i < mv->tl_count; i++) {
            mv->tl_wx[i] = map_geo_lon_to_world_px(mv->tl_lons[i], mv->zoom);
            mv->tl_wy[i] = map_geo_lat_to_world_py(mv->tl_lats[i], mv->zoom);
        }
        mv->tl_zoom = mv->zoom;
    }

    for (int i = 0; i < mv->tl_count; i++) {
        double wx = mv->tl_wx[i];
        double wy = mv->tl_wy[i];

        double dx = wx - ctx->center_wx;
        double dy = wy - ctx->center_wy;
        if (fabs(dx) > 200.0 || fabs(dy) > 200.0) continue;

        double sx, sy;
        mv_world_to_screen(ctx, wx, wy, &sx, &sy);

        /* Outer pill badge: 26x14px, radius 7px, navy bg #1C2740, white border */
        int32_t isx = (int32_t)lround(sx);
        int32_t isy = (int32_t)lround(sy);

        lv_area_t a = {
            .x1 = isx - 13,
            .y1 = isy - 7,
            .x2 = isx + 13,
            .y2 = isy + 7,
        };
        if (!mv_segment_visible(&ctx->content, a.x1, a.y1, a.x2, a.y2, 0)) continue;

        lv_draw_rect_dsc_t pill_dsc;
        lv_draw_rect_dsc_init(&pill_dsc);
        pill_dsc.radius = 7;
        pill_dsc.bg_color = lv_color_hex(0x1C2740);
        pill_dsc.bg_opa = LV_OPA_COVER;
        pill_dsc.border_color = lv_color_white();
        pill_dsc.border_width = 1;
        pill_dsc.border_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &pill_dsc, &a);

        /* Three signal dots: Red (#F0453A), Amber (#F6C23E), Green (#36B36A) */
        lv_draw_rect_dsc_t dot_dsc;
        lv_draw_rect_dsc_init(&dot_dsc);
        dot_dsc.radius = LV_RADIUS_CIRCLE;
        dot_dsc.bg_opa = LV_OPA_COVER;

        /* Red dot (left) */
        lv_area_t dot_r = { .x1 = isx - 9, .y1 = isy - 2, .x2 = isx - 5, .y2 = isy + 2 };
        dot_dsc.bg_color = lv_color_hex(0xF0453A);
        lv_draw_rect(layer, &dot_dsc, &dot_r);

        /* Yellow dot (center) */
        lv_area_t dot_y = { .x1 = isx - 2, .y1 = isy - 2, .x2 = isx + 2, .y2 = isy + 2 };
        dot_dsc.bg_color = lv_color_hex(0xF6C23E);
        lv_draw_rect(layer, &dot_dsc, &dot_y);

        /* Green dot (right) */
        lv_area_t dot_g = { .x1 = isx + 5, .y1 = isy - 2, .x2 = isx + 9, .y2 = isy + 2 };
        dot_dsc.bg_color = lv_color_hex(0x36B36A);
        lv_draw_rect(layer, &dot_dsc, &dot_g);
    }
}

/**
 * The tether back to the route, and an arrow when the road is off screen.
 *
 * Drawn above the route so it reads as an instruction rather than another road,
 * and dashed so it is obviously not one.
 */
static void mv_draw_return(map_view_t *mv, lv_layer_t *layer, const render_ctx_t *ctx)
{
    if (!mv->return_active || !mv->pos_valid) return;

    double fx, fy, tx, ty;
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(mv->dpos_lon, mv->zoom),
                       map_geo_lat_to_world_py(mv->dpos_lat, mv->zoom), &fx, &fy);
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(mv->return_lon, mv->zoom),
                       map_geo_lat_to_world_py(mv->return_lat, mv->zoom), &tx, &ty);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color      = mv->style.return_color;
    dsc.width      = 5;
    dsc.opa        = LV_OPA_COVER;
    dsc.dash_width = 12;
    dsc.dash_gap   = 9;
    dsc.round_start = 1;
    dsc.round_end   = 1;
    dsc.p1.x = (lv_value_precise_t)lround(fx);
    dsc.p1.y = (lv_value_precise_t)lround(fy);
    dsc.p2.x = (lv_value_precise_t)lround(tx);
    dsc.p2.y = (lv_value_precise_t)lround(ty);
    lv_draw_line(layer, &dsc);

    /* If the road is off screen the dashes run off the edge and say nothing
     * about how far. Put an arrow where the line leaves the view. */
    bool on_screen = tx >= ctx->content.x1 && tx <= ctx->content.x2 &&
                     ty >= ctx->content.y1 && ty <= ctx->content.y2;
    if (on_screen) return;

    double cx = (ctx->content.x1 + ctx->content.x2) / 2.0;
    double cy = (ctx->content.y1 + ctx->content.y2) / 2.0;
    double dx = tx - cx, dy = ty - cy;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1.0) return;
    dx /= len;
    dy /= len;

    /* The panel is round, so the edge is a circle inset by the arrow's reach
     * rather than the corner of a rectangle. */
    double radius = (lv_area_get_width(&ctx->content) < lv_area_get_height(&ctx->content)
                     ? lv_area_get_width(&ctx->content)
                     : lv_area_get_height(&ctx->content)) / 2.0 - MV_EDGE_ARROW_INSET;
    double ax = cx + dx * radius;
    double ay = cy + dy * radius;
    double px = -dy, py = dx;              /* perpendicular, for the base */

    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.bg_color = mv->style.return_color;
    tri.bg_opa   = LV_OPA_COVER;
    tri.p[0].x = (lv_value_precise_t)lround(ax + dx * 16);
    tri.p[0].y = (lv_value_precise_t)lround(ay + dy * 16);
    tri.p[1].x = (lv_value_precise_t)lround(ax - dx * 8 + px * 12);
    tri.p[1].y = (lv_value_precise_t)lround(ay - dy * 8 + py * 12);
    tri.p[2].x = (lv_value_precise_t)lround(ax - dx * 8 - px * 12);
    tri.p[2].y = (lv_value_precise_t)lround(ay - dy * 8 - py * 12);
    lv_draw_triangle(layer, &tri);
}

static void mv_draw_position(map_view_t *mv, lv_layer_t *layer, const render_ctx_t *ctx)
{
    if (!mv->pos_valid) return;

    double sx, sy;
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(mv->dpos_lon, mv->zoom),
                       map_geo_lat_to_world_py(mv->dpos_lat, mv->zoom),
                       &sx, &sy);

    int32_t cx = (int32_t)lround(sx);
    int32_t cy = (int32_t)lround(sy);
    int32_t r  = mv->style.marker_radius;

    if (!mv_segment_visible(&ctx->content, cx - r - 70, cy - r - 70, cx + r + 70, cy + r + 70, 0)) {
        return;
    }

    double th = (mv->pos_heading + mv->rotation_deg) * M_PI / 180.0;

    /* 1. AutoNavi-style translucent compass ring with cardinal direction labels (北/东/南/西) */
    int32_t cr = 42;
    lv_draw_rect_dsc_t ring_dsc;
    lv_draw_rect_dsc_init(&ring_dsc);
    ring_dsc.radius       = LV_RADIUS_CIRCLE;
    ring_dsc.bg_color     = lv_color_hex(0x0284C7);
    ring_dsc.bg_opa       = LV_OPA_20;
    ring_dsc.border_color = lv_color_hex(0x38BDF8);
    ring_dsc.border_width = 1;
    ring_dsc.border_opa   = LV_OPA_40;
    lv_area_t ring_a = { .x1 = cx - cr, .y1 = cy - cr, .x2 = cx + cr, .y2 = cy + cr };
    lv_draw_rect(layer, &ring_dsc, &ring_a);

    /* Cardinal direction labels on compass ring perimeter */
    double rot_rad = mv->rotation_deg * M_PI / 180.0;
    const char *cardinals[4] = {"北", "东", "南", "西"};
    lv_color_t card_colors[4] = {
        lv_color_hex(0xEF4444), /* North: High-visibility Red */
        lv_color_hex(0xCBD5E1), /* East: Slate */
        lv_color_hex(0xCBD5E1), /* South: Slate */
        lv_color_hex(0xCBD5E1)  /* West: Slate */
    };

    lv_draw_label_dsc_t card_lbl_dsc;
    lv_draw_label_dsc_init(&card_lbl_dsc);
    card_lbl_dsc.font  = &font_chinese_16;
    card_lbl_dsc.opa   = LV_OPA_COVER;
    card_lbl_dsc.align = LV_TEXT_ALIGN_CENTER;

    double label_r = 41.0;
    for (int i = 0; i < 4; i++) {
        double ang = rot_rad + (double)i * (M_PI * 0.5);
        int32_t lx = (int32_t)lround(cx + sin(ang) * label_r);
        int32_t ly = (int32_t)lround(cy - cos(ang) * label_r);
        lv_area_t la = { .x1 = lx - 9, .y1 = ly - 9, .x2 = lx + 9, .y2 = ly + 9 };
        card_lbl_dsc.color = card_colors[i];
        card_lbl_dsc.text = cardinals[i];
        lv_draw_label(layer, &card_lbl_dsc, &la);
    }

    /* 2. Forward directional illumination cone (AMap style light beam, bold 46px) */
    double cone_len = 58.0;
    double cone_half = 0.35; /* ~20 deg */
    lv_draw_triangle_dsc_t cone;
    lv_draw_triangle_dsc_init(&cone);
    cone.bg_color = lv_color_hex(0x38BDF8);
    cone.bg_opa   = LV_OPA_20;
    cone.p[0].x = (lv_value_precise_t)cx;
    cone.p[0].y = (lv_value_precise_t)cy;
    cone.p[1].x = (lv_value_precise_t)lround(cx + sin(th - cone_half) * cone_len);
    cone.p[1].y = (lv_value_precise_t)lround(cy - cos(th - cone_half) * cone_len);
    cone.p[2].x = (lv_value_precise_t)lround(cx + sin(th + cone_half) * cone_len);
    cone.p[2].y = (lv_value_precise_t)lround(cy - cos(th + cone_half) * cone_len);
    lv_draw_triangle(layer, &cone);

    /* 3. Navigation arrow: pre-rendered anti-aliased sprite, rotated by LVGL. */
    const lv_image_dsc_t *sprite = mv_arrow_sprite(mv->off_route);
    if (sprite) {
        double deg = fmod(mv->pos_heading + mv->rotation_deg, 360.0);
        if (deg < 0.0) deg += 360.0;

        lv_draw_image_dsc_t img;
        lv_draw_image_dsc_init(&img);
        img.src       = sprite;
        img.antialias = 1;
        img.rotation  = (int32_t)lround(deg * 10.0) % 3600;
        img.pivot.x   = ARROW_SPRITE_PX / 2;
        img.pivot.y   = ARROW_SPRITE_PX / 2;

        lv_area_t a = {
            .x1 = cx - ARROW_SPRITE_PX / 2,
            .y1 = cy - ARROW_SPRITE_PX / 2,
            .x2 = cx + ARROW_SPRITE_PX / 2 - 1,
            .y2 = cy + ARROW_SPRITE_PX / 2 - 1,
        };
        lv_draw_image(layer, &img, &a);
    }
}

static void mv_draw_debug(map_view_t *mv, lv_layer_t *layer, const render_ctx_t *ctx,
                          int route_segments)
{
    map_tile_cache_stats_t st;
    map_tile_cache_get_stats(mv->cache, &st);

    char buf[96];
    snprintf(buf, sizeof(buf), "z%d  tiles %d/%d ld%d  hit %lu/%lu  seg %d",
             mv->zoom, st.resident, st.capacity, st.loading,
             (unsigned long)st.hits, (unsigned long)(st.hits + st.misses),
             route_segments);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text       = buf;
    dsc.text_local = 1;   /* LVGL copies the string; `buf` is on our stack */
    dsc.color      = lv_color_white();
    dsc.opa        = LV_OPA_COVER;
    dsc.align      = LV_TEXT_ALIGN_CENTER;

    lv_area_t a = {
        .x1 = ctx->content.x1,
        .y1 = ctx->content.y1 + 4,
        .x2 = ctx->content.x2,
        .y2 = ctx->content.y1 + 24,
    };

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa   = LV_OPA_50;
    lv_draw_rect(layer, &bg, &a);

    lv_draw_label(layer, &dsc, &a);
}

static void mv_draw(map_view_t *mv, lv_event_t *e)
{
    if (!mv->cache) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer) return;

    render_ctx_t ctx;
    mv_make_ctx(mv, &ctx);

    map_tile_cache_begin_frame(mv->cache);
    mv_draw_tiles(mv, layer, &ctx);

    int segments = 0;
    if (mv->route) {
        segments = mv_draw_route(mv, layer, &ctx);
        mv_draw_origin(mv, layer, &ctx);
        mv_draw_destination(mv, layer, &ctx);
        mv_draw_traffic_lights(mv, layer, &ctx);

        if (segments >= MAX_VIS_SEGS && !mv->warned_segment_cap) {
            mv->warned_segment_cap = true;
            ESP_LOGW(TAG, "Route hit the %d segment cap; raise the exporter's "
                          "simplification tolerance for this zoom", ROUTE_MAX_SEGMENTS);
        }
    }

    mv_draw_return(mv, layer, &ctx);
    mv_draw_position(mv, layer, &ctx);

    if (mv->debug) mv_draw_debug(mv, layer, &ctx, segments);
}

/* ----------------------------------------------------------------- input */

static void mv_user_took_over(map_view_t *mv)
{
    if (mv->follow) {
        mv->follow = false;
        lv_obj_send_event(mv->obj, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

static void mv_handle_input(map_view_t *mv, lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        mv->pressing     = true;
        mv->drag_active  = false;
        mv->press_travel = 0;
        mv->vel_x = mv->vel_y = 0;
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;

        lv_point_t vect;
        lv_indev_get_vect(indev, &vect);
        if (vect.x == 0 && vect.y == 0) return;

        mv->press_travel += LV_ABS(vect.x) + LV_ABS(vect.y);
        if (!mv->drag_active && mv->press_travel < DRAG_START_PX) return;

        if (!mv->drag_active) {
            mv->drag_active = true;
            mv_user_took_over(mv);
        }

        /* Dragging moves the map with the finger, so the centre goes the
         * opposite way. Account for map rotation if active. */
        double dx = vect.x;
        double dy = vect.y;
        if (fabs(mv->rotation_deg) > 0.05) {
            double c = cos(mv->rotation_rad);
            double s = sin(mv->rotation_rad);
            dx = vect.x * c + vect.y * s;
            dy = -vect.x * s + vect.y * c;
        }

        mv->center_wx -= dx;
        mv->center_wy -= dy;
        mv_clamp_center(mv);

        mv->vel_x = -dx;
        mv->vel_y = -dy;

        lv_obj_invalidate(mv->obj);
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        mv->pressing = false;
        if (!mv->drag_active) {
            mv->vel_x = mv->vel_y = 0;
        }
        mv->drag_active = false;
        return;
    }
}

/* ------------------------------------------------------------- refreshing */

/**
 * Pre-load the few tiles under the road ahead of the rider.
 *
 * Walks the cached route projection (cheap float maths, no trig) from the
 * rider's position for ROUTE_AHEAD_PX world pixels and queues each distinct tile
 * once. That is a constant handful of tiles at any zoom. Background priority:
 * the loader serves tiles on screen first.
 */
#define ROUTE_AHEAD_PX      900.0f
#define ROUTE_AHEAD_TILES   6
#define ROUTE_PREFETCH_MS   1000

static void mv_prefetch_route(map_view_t *mv)
{
    if (!mv->cache || !mv->route || !mv_pj_ensure(mv)) return;

    map_tile_cache_stats_t st;
    map_tile_cache_get_stats(mv->cache, &st);
    if (st.capacity < 10) return;                   /* too small to hold the view plus anything ahead */

    int fetch_zoom = mv->zoom > 18 ? 18 : mv->zoom;
    int mult = (mv->zoom <= 18) ? 1 : (mv->zoom == 19 ? 2 : (mv->zoom == 20 ? 5 : 8));
    const double tile_disp = (double)TILE_PX * mult;
    const int n = 1 << fetch_zoom;

    const int count = mv->pj_count;
    int i = mv->prog_seg >= 0 ? mv->prog_seg : 0;
    if (i >= count - 1) return;

    int seen_x[ROUTE_AHEAD_TILES + 2], seen_y[ROUTE_AHEAD_TILES + 2], seen = 0;
    float walked = 0.0f;
    float px = mv->pj_x[i], py = mv->pj_y[i];

    for (; i < count - 1 && walked < ROUTE_AHEAD_PX && seen < ROUTE_AHEAD_TILES; i++) {
        float ex = mv->pj_x[i + 1], ey = mv->pj_y[i + 1];
        float len = sqrtf((ex - px) * (ex - px) + (ey - py) * (ey - py));
        int steps = (int)ceilf(len / 96.0f);
        if (steps < 1) steps = 1;

        for (int k = 1; k <= steps && seen < ROUTE_AHEAD_TILES; k++) {
            float t = (float)k / steps;
            double wx = mv->pj_ox + px + (ex - px) * t;
            double wy = mv->pj_oy + py + (ey - py) * t;
            int tx = (int)floor(wx / tile_disp);
            int ty = (int)floor(wy / tile_disp);
            if (ty < 0 || ty >= n) continue;
            tx = ((tx % n) + n) % n;

            bool dup = false;
            for (int j = 0; j < seen; j++) if (seen_x[j] == tx && seen_y[j] == ty) { dup = true; break; }
            if (dup) continue;
            seen_x[seen] = tx;
            seen_y[seen] = ty;
            seen++;
            map_tile_cache_prefetch(mv->cache, fetch_zoom, tx, ty);
        }
        walked += len;
        px = ex;
        py = ey;
    }
}

static void mv_timer_cb(lv_timer_t *t)
{
    map_view_t *mv = (map_view_t *)lv_timer_get_user_data(t);
    if (!mv) return;

    bool dirty = false;

    /* Once a second, while the user is not panning, load the road ahead. */
    if (!mv->pressing && fabs(mv->vel_x) < 0.5 && fabs(mv->vel_y) < 0.5 &&
        mv->route && lv_tick_elaps(mv->route_prefetch_tick) > ROUTE_PREFETCH_MS) {
        mv->route_prefetch_tick = lv_tick_get();
        mv_prefetch_route(mv);
    }

    /* A background tile arrived: repaint. */
    if (mv->cache) {
        uint32_t v = map_tile_cache_get_version(mv->cache);
        if (v != mv->cache_version) {
            mv->cache_version = v;
            dirty = true;
        }
    }

    /* Fling after a drag. */
    if (!mv->pressing && (fabs(mv->vel_x) > MOMENTUM_STOP_PX || fabs(mv->vel_y) > MOMENTUM_STOP_PX)) {
        mv->center_wx += mv->vel_x;
        mv->center_wy += mv->vel_y;
        mv->vel_x *= MOMENTUM_DECAY;
        mv->vel_y *= MOMENTUM_DECAY;
        mv_clamp_center(mv);
        dirty = true;
    }

    /* Glide the drawn marker toward the latest (possibly route-snapped) fix, so
     * a 1 Hz GPS feed still moves smoothly. */
    if (mv->pos_valid) {
        double dla = mv->tgt_lat - mv->dpos_lat;
        double dlo = mv->tgt_lon - mv->dpos_lon;
        if (fabs(dla) > POS_SNAP_DEG || fabs(dlo) > POS_SNAP_DEG) {
            mv->dpos_lat += dla * POS_EASE;
            mv->dpos_lon += dlo * POS_EASE;
            dirty = true;
        } else if (dla != 0.0 || dlo != 0.0) {
            mv->dpos_lat = mv->tgt_lat;
            mv->dpos_lon = mv->tgt_lon;
            dirty = true;
        }
    }

    /* Camera: the marker is already eased, so once we are near it just lock on
     * (the marker then stays put on screen and the map slides under it). Only a
     * large gap - e.g. after the user dragged away - is eased. */
    if (mv->follow) {
        double tx, ty;
        if (mv_follow_target(mv, &tx, &ty)) {
            double dx = tx - mv->center_wx;
            double dy = ty - mv->center_wy;
            if (fabs(dx) > FOLLOW_LOCK_PX || fabs(dy) > FOLLOW_LOCK_PX) {
                mv->center_wx += dx * FOLLOW_EASE;
                mv->center_wy += dy * FOLLOW_EASE;
                mv_clamp_center(mv);
                dirty = true;
            } else if (dx != 0.0 || dy != 0.0) {
                mv->center_wx = tx;
                mv->center_wy = ty;
                mv_clamp_center(mv);
                dirty = true;
            }
        }
    }

    if (dirty) lv_obj_invalidate(mv->obj);
}

static void mv_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
        case LV_EVENT_DRAW_MAIN:
            mv_draw(mv, e);
            break;

        case LV_EVENT_PRESSED:
        case LV_EVENT_PRESSING:
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            mv_handle_input(mv, e);
            break;

        case LV_EVENT_DELETE:
            if (mv->timer) lv_timer_delete(mv->timer);
            if (mv->cache) map_tile_cache_destroy(mv->cache);
            mv_pj_free(mv);
            lv_obj_set_user_data(obj, NULL);
            free(mv);
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------ public API */

lv_obj_t *map_view_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;

    map_view_t *mv = (map_view_t *)calloc(1, sizeof(map_view_t));
    if (!mv) {
        ESP_LOGE(TAG, "Out of memory for the map view");
        lv_obj_delete(obj);
        return NULL;
    }

    mv->obj      = obj;
    mv->zoom     = 18;
    mv->min_zoom = 10;
    mv->max_zoom = 20;
    mv->follow_offset = 0;
    mv->prog_seg = -1;
    mv->tl_zoom  = -1;
    mv_default_style(&mv->style);

    /* Start on the prime meridian at the equator; the app will move us. */
    mv->center_wx = map_geo_lon_to_world_px(0.0, mv->zoom);
    mv->center_wy = map_geo_lat_to_world_py(0.0, mv->zoom);

    lv_obj_set_user_data(obj, mv);

    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xE8E4DD), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(obj, mv_event_cb, LV_EVENT_ALL, NULL);

    mv->timer = lv_timer_create(mv_timer_cb, REFRESH_PERIOD_MS, mv);

    return obj;
}

bool map_view_set_tile_source(lv_obj_t *obj, const char *base_path, const char *folder,
                              int cache_capacity, bool use_spiram)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return false;

    if (mv->cache) {
        map_tile_cache_destroy(mv->cache);
        mv->cache = NULL;
    }

    map_tile_cache_config_t cfg = {
        .base_path  = base_path,
        .folder     = folder,
        .capacity   = cache_capacity,
        .use_spiram = use_spiram,
    };
    mv->cache = map_tile_cache_create(&cfg);
    if (!mv->cache) return false;

    mv->cache_version = 0;
    lv_obj_invalidate(obj);
    return true;
}

void map_view_set_tile_folder(lv_obj_t *obj, const char *folder)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || !mv->cache) return;
    map_tile_cache_set_folder(mv->cache, folder);
    lv_obj_invalidate(obj);
}

map_tile_cache_handle_t map_view_get_tile_cache(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv->cache : NULL;
}

void map_view_set_zoom(lv_obj_t *obj, int zoom)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    if (zoom < mv->min_zoom) zoom = mv->min_zoom;
    if (zoom > mv->max_zoom) zoom = mv->max_zoom;
    if (zoom == mv->zoom) return;

    if (mv->follow && mv->pos_valid) {
        mv->zoom = zoom;
        mv->center_wx = map_geo_lon_to_world_px(mv->dpos_lon, zoom);
        mv->center_wy = map_geo_lat_to_world_py(mv->dpos_lat, zoom);
    } else {
        mv_reproject_center(mv, zoom);
    }
    mv->vel_x = mv->vel_y = 0;
    mv->warned_segment_cap = false;
    lv_obj_invalidate(obj);
}

int map_view_get_zoom(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv->zoom : 0;
}

void map_view_set_zoom_range(lv_obj_t *obj, int min_zoom, int max_zoom)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || min_zoom > max_zoom) return;

    mv->min_zoom = min_zoom;
    mv->max_zoom = max_zoom;
    map_view_set_zoom(obj, mv->zoom);
}

void map_view_zoom_in(lv_obj_t *obj)  { map_view_set_zoom(obj, map_view_get_zoom(obj) + 1); }
void map_view_zoom_out(lv_obj_t *obj) { map_view_set_zoom(obj, map_view_get_zoom(obj) - 1); }

void map_view_set_center(lv_obj_t *obj, double lat, double lon)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    mv->center_wx = map_geo_lon_to_world_px(lon, mv->zoom);
    mv->center_wy = map_geo_lat_to_world_py(lat, mv->zoom);
    mv->vel_x = mv->vel_y = 0;
    mv_clamp_center(mv);
    lv_obj_invalidate(obj);
}

void map_view_get_center(lv_obj_t *obj, double *lat, double *lon)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;
    if (lat) *lat = map_geo_world_py_to_lat(mv->center_wy, mv->zoom);
    if (lon) *lon = map_geo_world_px_to_lon(mv->center_wx, mv->zoom);
}

void map_view_fit_bounds(lv_obj_t *obj,
                         double min_lat, double min_lon,
                         double max_lat, double max_lon,
                         int padding_px)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    lv_area_t content;
    mv_content_area(mv, &content);
    int32_t w = lv_area_get_width(&content) - 2 * padding_px;
    int32_t h = lv_area_get_height(&content) - 2 * padding_px;
    if (w < 16) w = 16;
    if (h < 16) h = 16;

    int best = mv->min_zoom;
    for (int z = mv->max_zoom; z >= mv->min_zoom; z--) {
        double x1 = map_geo_lon_to_world_px(min_lon, z);
        double x2 = map_geo_lon_to_world_px(max_lon, z);
        double y1 = map_geo_lat_to_world_py(max_lat, z);   /* north = smaller y */
        double y2 = map_geo_lat_to_world_py(min_lat, z);

        if (fabs(x2 - x1) <= w && fabs(y2 - y1) <= h) {
            best = z;
            break;
        }
    }

    mv->zoom = best;
    mv->center_wx = (map_geo_lon_to_world_px(min_lon, best) +
                     map_geo_lon_to_world_px(max_lon, best)) / 2.0;
    mv->center_wy = (map_geo_lat_to_world_py(min_lat, best) +
                     map_geo_lat_to_world_py(max_lat, best)) / 2.0;
    mv->vel_x = mv->vel_y = 0;
    mv_clamp_center(mv);
    lv_obj_invalidate(obj);
}

void map_view_set_route(lv_obj_t *obj, map_route_handle_t route)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;
    mv->route = route;
    mv->traveled_m = 0;
    mv->prog_seg = -1;
    mv->prog_t = 0.0;
    mv->snapped = false;
    mv->pj_route = NULL;            /* force a re-projection */
    mv->pj_count = 0;
    if (route) map_route_reset_match(route);
    else mv_pj_free(mv);
    mv->warned_segment_cap = false;
    lv_obj_invalidate(obj);
}

void map_view_set_traffic_lights(lv_obj_t *obj, const double *lats, const double *lons, int count)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;
    if (!lats || !lons || count <= 0) {
        mv->tl_count = 0;
    } else {
        if (count > 32) count = 32;
        mv->tl_count = count;
        for (int i = 0; i < count; i++) {
            mv->tl_lats[i] = lats[i];
            mv->tl_lons[i] = lons[i];
        }
    }
    mv->tl_zoom = -1;
    lv_obj_invalidate(obj);
}

void map_view_set_progress(lv_obj_t *obj, uint32_t traveled_m)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->traveled_m == traveled_m) return;
    mv->traveled_m = traveled_m;

    /* Turn a distance into (segment, t) so the grey/blue split lands mid-segment. */
    int n = mv->route ? map_route_get_point_count(mv->route) : 0;
    if (n >= 2) {
        int lo = 0, hi = n - 2;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            map_route_point_t p;
            if (!map_route_get_point(mv->route, mid, &p) || p.cum_dist_m > traveled_m) hi = mid - 1;
            else lo = mid;
        }
        map_route_point_t a, b;
        double t = 0.0;
        if (map_route_get_point(mv->route, lo, &a) && map_route_get_point(mv->route, lo + 1, &b) &&
            b.cum_dist_m > a.cum_dist_m && traveled_m > a.cum_dist_m) {
            t = (double)(traveled_m - a.cum_dist_m) / (double)(b.cum_dist_m - a.cum_dist_m);
            if (t > 1.0) t = 1.0;
        }
        mv->prog_seg = lo;
        mv->prog_t = t;
    }
    lv_obj_invalidate(obj);
}

uint32_t map_view_get_traveled_m(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv->traveled_m : 0;
}

void map_view_set_position(lv_obj_t *obj, double lat, double lon,
                           double heading_deg, bool valid)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    if (mv->pos_valid == valid && mv->pos_lat == lat && mv->pos_lon == lon &&
        mv->pos_heading == heading_deg) {
        return;   /* same fix replayed; nothing to redraw */
    }

    bool was_valid = mv->pos_valid;
    mv->pos_lat     = lat;
    mv->pos_lon     = lon;
    mv->pos_heading = heading_deg;
    mv->pos_valid   = valid;

    double tlat = lat, tlon = lon;
    bool snapped = false;

    if (valid && mv->route) {
        map_route_match_t m;
        if (map_route_match(mv->route, lat, lon, PROGRESS_TOL_M, &m)) {
            /* Close enough to the route (with hysteresis so the marker does not
             * flap on the edge): sit exactly on the line and follow it, like AMap
             * does for a rider. */
            double limit = mv->snapped ? SNAP_EXIT_M : SNAP_ENTER_M;
            if (m.cross_track_m <= limit) {
                snapped = true;
                tlat = m.snapped_lat;
                tlon = m.snapped_lon;
            }

            /* Progress only moves forward (GPS jitter must not un-grey the road),
             * unless the match jumps far back, which means a new lap/route. */
            if (m.on_route) {
                bool forward = (m.segment_index > mv->prog_seg) ||
                               (m.segment_index == mv->prog_seg && m.t >= mv->prog_t);
                bool far_back = (mv->traveled_m > m.traveled_m + 150u);
                if (mv->prog_seg < 0 || forward || far_back) {
                    mv->prog_seg   = m.segment_index;
                    mv->prog_t     = m.t;
                    mv->traveled_m = m.traveled_m;
                }
            }
        }
    }
    mv->snapped = snapped;
    mv->tgt_lat = tlat;
    mv->tgt_lon = tlon;

    if (valid && (!was_valid ||
                  fabs(tlat - mv->dpos_lat) + fabs(tlon - mv->dpos_lon) > POS_JUMP_DEG)) {
        mv->dpos_lat = tlat;
        mv->dpos_lon = tlon;
        if (mv->follow) {            /* the marker teleported (first fix / big jump): so does the camera */
            mv->center_wx = map_geo_lon_to_world_px(tlon, mv->zoom);
            mv->center_wy = map_geo_lat_to_world_py(tlat, mv->zoom);
            mv->vel_x = mv->vel_y = 0;
            mv_clamp_center(mv);
        }
    }

    lv_obj_invalidate(obj);
}

void map_view_set_off_route(lv_obj_t *obj, bool off_route)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->off_route == off_route) return;
    mv->off_route = off_route;
    lv_obj_invalidate(obj);
}

void map_view_set_return_target(lv_obj_t *obj, double lat, double lon, bool active)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    if (mv->return_active == active &&
        mv->return_lat == lat && mv->return_lon == lon) {
        return;                 /* same target replayed; nothing to redraw */
    }

    mv->return_active = active;
    mv->return_lat    = lat;
    mv->return_lon    = lon;
    lv_obj_invalidate(obj);
}

void map_view_set_follow(lv_obj_t *obj, bool follow)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->follow == follow) return;

    mv->follow = follow;
    if (follow) {
        mv->vel_x = mv->vel_y = 0;
    }
    lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
}

bool map_view_get_follow(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv->follow : false;
}

void map_view_set_follow_offset(lv_obj_t *obj, int32_t offset_px)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;
    mv->follow_offset = offset_px;
}

void map_view_set_rotation(lv_obj_t *obj, double angle_deg)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    while (angle_deg > 180.0) angle_deg -= 360.0;
    while (angle_deg <= -180.0) angle_deg += 360.0;

    if (fabs(mv->rotation_deg - angle_deg) < 0.1) return;

    mv->rotation_deg = angle_deg;
    mv->rotation_rad = angle_deg * M_PI / 180.0;
    lv_obj_invalidate(obj);
}

double map_view_get_rotation(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv->rotation_deg : 0.0;
}

void map_view_orient_to_route(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    // Center on position if valid
    if (mv->pos_valid) {
        mv->center_wx = map_geo_lon_to_world_px(mv->dpos_lon, mv->zoom);
        mv->center_wy = map_geo_lat_to_world_py(mv->dpos_lat, mv->zoom);
        mv->vel_x = mv->vel_y = 0;
        mv_clamp_center(mv);
    }

    if (!mv->route || map_route_get_point_count(mv->route) < 2) {
        map_view_set_rotation(obj, 0.0);
        return;
    }

    int n = map_route_get_point_count(mv->route);
    int best_seg = 0;

    if (mv->prog_seg >= 0 && mv->prog_seg < n - 1) {
        best_seg = mv->prog_seg;        /* already map-matched; no need to scan */
    } else if (mv->pos_valid) {
        double min_d2 = 1e18;
        for (int i = 0; i < n - 1; i++) {
            map_route_point_t p1, p2;
            if (!map_route_get_point(mv->route, i, &p1)) break;
            if (!map_route_get_point(mv->route, i + 1, &p2)) break;

            double mid_lat = (p1.lat + p2.lat) * 0.5;
            double mid_lon = (p1.lon + p2.lon) * 0.5;
            double dlat = mv->pos_lat - mid_lat;
            double dlon = mv->pos_lon - mid_lon;
            double d2 = dlat * dlat + dlon * dlon;
            if (d2 < min_d2) {
                min_d2 = d2;
                best_seg = i;
            }
        }
    }

    map_route_point_t p_start, p_next;
    if (map_route_get_point(mv->route, best_seg, &p_start) &&
        map_route_get_point(mv->route, best_seg + 1, &p_next)) {
        double wx0 = map_geo_lon_to_world_px(p_start.lon, mv->zoom);
        double wy0 = map_geo_lat_to_world_py(p_start.lat, mv->zoom);
        double wx1 = map_geo_lon_to_world_px(p_next.lon, mv->zoom);
        double wy1 = map_geo_lat_to_world_py(p_next.lat, mv->zoom);

        double dx = wx1 - wx0;
        double dy = wy1 - wy0;
        double bearing_deg = atan2(dx, -dy) * 180.0 / M_PI;
        if (bearing_deg < 0.0) bearing_deg += 360.0;

        // Rotate by -bearing_deg so this road segment points straight UP
        map_view_set_rotation(obj, -bearing_deg);
    }
}

bool map_view_route_bearing_ahead(lv_obj_t *obj, double ahead_px, double *out_deg)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || !out_deg || !mv->route || !mv->snapped || mv->prog_seg < 0) return false;
    const int n = map_route_get_point_count(mv->route);
    if (mv->prog_seg >= n - 1) return false;

    map_route_point_t a, b;
    if (!map_route_get_point(mv->route, mv->prog_seg, &a) || !map_route_get_point(mv->route, mv->prog_seg + 1, &b)) return false;
    double ax = map_geo_lon_to_world_px(a.lon, mv->zoom), ay = map_geo_lat_to_world_py(a.lat, mv->zoom);
    double bx = map_geo_lon_to_world_px(b.lon, mv->zoom), by = map_geo_lat_to_world_py(b.lat, mv->zoom);

    const double sx = ax + mv->prog_t * (bx - ax);          /* where the rider is on the route */
    const double sy = ay + mv->prog_t * (by - ay);
    double px = sx, py = sy;                                /* walk forward from here */
    double nx = bx, ny = by;
    double left = ahead_px;
    int i = mv->prog_seg + 1;                               /* index of the vertex we walk to */

    for (;;) {
        double len = hypot(nx - px, ny - py);
        if (len >= left) {
            if (len > 0.0) { px += (nx - px) * left / len; py += (ny - py) * left / len; }
            break;
        }
        left -= len;
        px = nx; py = ny;
        if (++i >= n) break;                                /* route ends: aim at its last point */
        map_route_point_t c;
        if (!map_route_get_point(mv->route, i, &c)) break;
        nx = map_geo_lon_to_world_px(c.lon, mv->zoom);
        ny = map_geo_lat_to_world_py(c.lat, mv->zoom);
    }

    const double dx = px - sx, dy = py - sy;
    if (hypot(dx, dy) < 4.0) return false;
    double deg = atan2(dx, -dy) * 180.0 / M_PI;
    if (deg < 0.0) deg += 360.0;
    *out_deg = deg;
    return true;
}

void map_view_set_style(lv_obj_t *obj, const map_view_style_t *style)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || !style) return;
    mv->style = *style;
    lv_obj_invalidate(obj);
}

void map_view_get_style(lv_obj_t *obj, map_view_style_t *out)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || !out) return;
    *out = mv->style;
}

void map_view_set_debug_overlay(lv_obj_t *obj, bool enabled)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->debug == enabled) return;
    mv->debug = enabled;
    lv_obj_invalidate(obj);
}

void map_view_screen_to_latlon(lv_obj_t *obj, int32_t x, int32_t y, double *lat, double *lon)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    render_ctx_t ctx;
    mv_make_ctx(mv, &ctx);

    double wx, wy;
    if (!ctx.has_rotation) {
        wx = ctx.origin_wx + (x - ctx.content.x1);
        wy = ctx.origin_wy + (y - ctx.content.y1 - ctx.anchor_offset_y);
    } else {
        double sx_rel = x - ctx.cx;
        double sy_rel = y - (ctx.cy + ctx.anchor_offset_y);
        double dx = sx_rel * ctx.cos_r + sy_rel * ctx.sin_r;
        double dy = -sx_rel * ctx.sin_r + sy_rel * ctx.cos_r;
        wx = ctx.center_wx + dx;
        wy = ctx.center_wy + dy;
    }

    if (lat) *lat = map_geo_world_py_to_lat(wy, mv->zoom);
    if (lon) *lon = map_geo_world_px_to_lon(wx, mv->zoom);
}

bool map_view_latlon_to_screen(lv_obj_t *obj, double lat, double lon, int32_t *x, int32_t *y)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return false;

    render_ctx_t ctx;
    mv_make_ctx(mv, &ctx);

    double sx, sy;
    mv_world_to_screen(&ctx,
                       map_geo_lon_to_world_px(lon, mv->zoom),
                       map_geo_lat_to_world_py(lat, mv->zoom),
                       &sx, &sy);

    if (x) *x = (int32_t)lround(sx);
    if (y) *y = (int32_t)lround(sy);

    return sx >= ctx.content.x1 && sx <= ctx.content.x2 &&
           sy >= ctx.content.y1 && sy <= ctx.content.y2;
}
