#include "rider.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------
 * A 72x44 "pixel" picture shown at 3x on a canvas (216x132): the rider (hand-typed sprite in
 * rider_sprite.inc, outlined and shaded) standing next to a yellow mountain bike (drawn with
 * lines/circles into the same grid so the wheels can turn). The rider breathes (1 px bob).
 * ---------------------------------------------------------------------------------------------- */
#define GW      72
#define GH      44
#define SCALE   3
#define CW      (GW * SCALE)
#define CH      (GH * SCALE)
#define FRAME_MS 120

enum {
    C_BG = 0, C_K, C_HAIR, C_HAIR_HI, C_SKIN, C_SKIN_SH, C_GLASS_HI,
    C_JKT, C_JKT_SH, C_JKT_HI, C_TEE, C_JEANS, C_JEANS_SH, C_SHOE, C_SHOE_SH,
    C_YEL, C_YEL_HI, C_YEL_SH, C_TIRE, C_RIM, C_SPOKE, C_DARK, C_DARK_HI, C_GROUND, C_SHADOW, C_COUNT
};

static const uint32_t s_pal[C_COUNT] = {
    0x000000,   /* background: black like the idle look of the unit */
    0x0A0A0F,   /* outline */
    0x2B1B12, 0x4A3322,                 /* hair, hair highlight */
    0xE3A878, 0xB87850,                 /* skin, skin shade */
    0x6B7A8A,                           /* glasses glint */
    0x2F855A, 0x1F5C3E, 0x4FB37E,       /* jacket, shade, highlight */
    0xF1F5F9,                           /* t-shirt */
    0x3B66C4, 0x284A94,                 /* jeans, shade */
    0xF1F5F9, 0x9AA7B8,                 /* shoes, shade */
    0xFFC800, 0xFFE55C, 0xC79A00,       /* bike frame, highlight, shade */
    0x20242B,                           /* tyre */
    0xB8C0CC, 0x8A94A3,                 /* rim, spokes */
    0x15181D, 0x3A4350,                 /* saddle/bars/forks, their highlight */
    0x2A3342, 0x151B26,                 /* ground, ground shadow */
};

/* The rider, 20 x 41. One letter = one pixel. */
#define HUM_W 20
#define HUM_H 41
static const char *const HUMAN[HUM_H] = {
#include "rider_sprite.inc"
};
#define HUM_X     0
#define HUM_Y     1
#define HUM_UPPER 21        /* rows above this move with the "breathing" bob */

/* Bike geometry (grid pixels). Ground is row 42. */
#define WHEEL_R   9
#define REAR_X    32
#define FRONT_X   60
#define AXLE_Y    32
#define BB_X      45
#define BB_Y      32

static uint8_t   s_grid[GH][GW];
static lv_obj_t *s_scr    = NULL;
static lv_obj_t *s_canvas = NULL;
static lv_draw_buf_t *s_buf = NULL;
static lv_timer_t *s_timer  = NULL;
static uint8_t   s_phase    = 0;

static void put(int x, int y, uint8_t c)
{
    if (x >= 0 && x < GW && y >= 0 && y < GH) s_grid[y][x] = c;
}

typedef void (*px_fn)(int x, int y, uint8_t c);

/** Bresenham; every pixel stamps a brush of @p w x @p w at its top-left. */
static void line_w(int x0, int y0, int x1, int y1, uint8_t c, int w, int off)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int j = 0; j < w; j++)
            for (int i = 0; i < w; i++) put(x0 + i + off, y0 + j + off, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void line(int x0, int y0, int x1, int y1, uint8_t c) { line_w(x0, y0, x1, y1, c, 1, 0); }

typedef struct { int8_t x0, y0, x1, y1; } tube_t;

static const tube_t TUBES[] = {
    { REAR_X, AXLE_Y, BB_X, BB_Y },         /* chain stay */
    { REAR_X, AXLE_Y, 40, 19 },             /* seat stay */
    { BB_X, BB_Y, 40, 17 },                 /* seat tube */
    { 40, 19, 55, 19 },                     /* top tube */
    { BB_X, BB_Y, 56, 22 },                 /* down tube */
    { 55, 17, 56, 22 },                     /* head tube */
    { 56, 22, 58, 27 },                     /* fork (upper) */
};

static void wheel(int cx, int cy, int phase)
{
    for (int y = -WHEEL_R - 1; y <= WHEEL_R + 1; y++) {
        for (int x = -WHEEL_R - 1; x <= WHEEL_R + 1; x++) {
            int d2 = x * x + y * y;
            if (d2 > 85) continue;
            if (d2 >= 70)      put(cx + x, cy + y, C_K);          /* outer edge of the fat tyre */
            else if (d2 >= 49) put(cx + x, cy + y, ((x + y) & 1) && d2 >= 58 ? C_DARK_HI : C_TIRE);   /* knobbly tread */
            else if (d2 >= 33) put(cx + x, cy + y, C_RIM);
        }
    }
    /* Six spokes, turning 10 degrees a frame (a full cycle every 6 frames looks like a spinning wheel). */
    for (int i = 0; i < 6; i++) {
        float a = (i * 60 + phase * 10) * (float)M_PI / 180.0f;
        line(cx, cy, cx + (int)lroundf(5.0f * cosf(a)), cy + (int)lroundf(5.0f * sinf(a)), C_SPOKE);
    }
    put(cx, cy, C_RIM); put(cx + 1, cy, C_RIM); put(cx, cy + 1, C_RIM); put(cx + 1, cy + 1, C_RIM);
}

static uint8_t human_color(char ch)
{
    switch (ch) {
    case 'K': return C_K;
    case 'H': return C_HAIR;     case 'h': return C_HAIR_HI;
    case 'S': return C_SKIN;     case 's': return C_SKIN_SH;
    case 'G': return C_K;        case 'g': return C_GLASS_HI;
    case 'J': return C_JKT;      case 'j': return C_JKT_SH;   case 'l': return C_JKT_HI;
    case 'T': return C_TEE;
    case 'P': return C_JEANS;    case 'p': return C_JEANS_SH;
    case 'W': return C_SHOE;     case 'w': return C_SHOE_SH;
    default:  return 255;
    }
}

static void draw_human(int bob)
{
    /* legs first, then the upper body over them, so the bob never opens a gap */
    for (int pass = 0; pass < 2; pass++) {
        int from = pass == 0 ? HUM_UPPER : 0, to = pass == 0 ? HUM_H : HUM_UPPER;
        int dy = pass == 0 ? 0 : bob;
        for (int y = from; y < to; y++) {
            for (int x = 0; x < HUM_W; x++) {
                uint8_t c = human_color(HUMAN[y][x]);
                if (c != 255) put(HUM_X + x, HUM_Y + y + dy, c);
            }
        }
    }
}

static void draw_bike(void)
{
    wheel(REAR_X, AXLE_Y, s_phase);
    wheel(FRONT_X, AXLE_Y, s_phase);

    /* frame: black outline for all tubes first (so tubes never cut into each other), then colour */
    for (size_t i = 0; i < sizeof(TUBES) / sizeof(TUBES[0]); i++) {
        const tube_t &t = TUBES[i];
        line_w(t.x0, t.y0, t.x1, t.y1, C_K, 4, -1);
    }
    for (size_t i = 0; i < sizeof(TUBES) / sizeof(TUBES[0]); i++) {
        const tube_t &t = TUBES[i];
        line_w(t.x0, t.y0, t.x1, t.y1, C_YEL_SH, 2, 0);          /* shade: the lower-right pixel of the brush */
    }
    for (size_t i = 0; i < sizeof(TUBES) / sizeof(TUBES[0]); i++) {
        const tube_t &t = TUBES[i];
        line_w(t.x0, t.y0, t.x1, t.y1, C_YEL, 2, 0);
        line(t.x0, t.y0, t.x1, t.y1, C_YEL_HI);                  /* highlight on the upper-left edge */
    }

    /* fork lower legs (dark), front and rear hubs */
    line_w(58, 27, FRONT_X, AXLE_Y, C_K, 4, -1);
    line_w(58, 27, FRONT_X, AXLE_Y, C_DARK, 2, 0);
    line(58, 27, FRONT_X, AXLE_Y, C_DARK_HI);

    /* saddle + post */
    line_w(40, 16, 40, 18, C_YEL, 2, 0);
    for (int x = 36; x <= 43; x++) { put(x, 15, C_DARK_HI); put(x, 16, C_DARK); put(x, 14, C_K); put(x, 17, x > 38 ? C_K : C_BG); }
    put(35, 15, C_K); put(35, 16, C_K); put(44, 15, C_K); put(44, 16, C_K);

    /* stem + flat bars + grips + brake lever */
    line_w(55, 14, 55, 17, C_DARK, 2, 0);
    line_w(52, 12, 60, 12, C_K, 4, -1);
    line_w(52, 12, 60, 12, C_DARK, 2, 0);
    line(52, 12, 60, 12, C_DARK_HI);
    put(60, 14, C_DARK); put(61, 14, C_DARK);

    /* chain ring, crank, pedal */
    for (int y = -3; y <= 3; y++)
        for (int x = -3; x <= 3; x++)
            if (x * x + y * y <= 9) put(BB_X + x, BB_Y + y, x * x + y * y <= 2 ? C_RIM : C_DARK);
    line_w(BB_X, BB_Y, BB_X - 3, BB_Y + 5, C_DARK, 2, 0);
    for (int x = -6; x <= -2; x++) { put(BB_X + x, BB_Y + 6, C_DARK); put(BB_X + x, BB_Y + 7, C_K); }
}

static void draw_scene(void)
{
    memset(s_grid, C_BG, sizeof(s_grid));
    for (int x = 0; x < GW; x++) { put(x, 42, C_GROUND); put(x, 43, C_SHADOW); }
    draw_human((s_phase / 3) & 1);          /* breathes once every 6 frames */
    draw_bike();
}

static void blit(void)
{
    if (!s_buf) return;
    uint16_t pal565[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) pal565[i] = lv_color_to_u16(lv_color_hex(s_pal[i]));

    uint32_t stride = s_buf->header.stride;
    for (int gy = 0; gy < GH; gy++) {
        uint16_t *row0 = (uint16_t *)((uint8_t *)s_buf->data + (uint32_t)(gy * SCALE) * stride);
        for (int gx = 0; gx < GW; gx++) {
            uint16_t c = pal565[s_grid[gy][gx]];
            for (int k = 0; k < SCALE; k++) row0[gx * SCALE + k] = c;
        }
        for (int k = 1; k < SCALE; k++) memcpy((uint8_t *)row0 + (uint32_t)k * stride, row0, CW * 2);
    }
    lv_obj_invalidate(s_canvas);
}

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    s_phase = (s_phase + 1) % 6;
    draw_scene();
    blit();
}

lv_obj_t *rider_init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(s_pal[C_BG]), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_buf = lv_draw_buf_create(CW, CH, LV_COLOR_FORMAT_RGB565, 0);
    s_canvas = lv_canvas_create(s_scr);
    if (s_buf) lv_canvas_set_draw_buf(s_canvas, s_buf);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 4);

    s_phase = 0;
    draw_scene();
    blit();

    s_timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
    lv_timer_pause(s_timer);
    return s_scr;
}

void rider_start(void)
{
    if (s_timer) lv_timer_resume(s_timer);
}

void rider_stop(void)
{
    if (s_timer) lv_timer_pause(s_timer);
}
