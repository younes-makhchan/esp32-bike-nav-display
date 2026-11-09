#include "pet.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>

/* ------------------------------------------------------------------------------------------------
 * Look of the eyes. Black screen, two rounded cyan blocks. "Eyelids" and the happy "cheek" are black
 * shapes laid over the eye, so they cost nothing extra and cannot show outside it (the parent clips).
 * ---------------------------------------------------------------------------------------------- */
#define SCREEN_SIZE 240
#define EYE_W       64
#define EYE_H       80
#define EYE_GAP     26
#define EYE_COLOR   0x5CE1FF
#define FRAME_MS    33

/* One pose of the face. Offsets are in pixels, scales are 1.0 = normal, lids/happy are 0..1. */
typedef struct {
    float x, y;         /* both eyes move together (looking around, bouncing) */
    float sw, sh;       /* eye width / height scale */
    float lidL, lidR;   /* how much of the eye the upper lid covers (1 = closed) */
    float hapL, hapR;   /* "smile": a black circle pushes up from the bottom (^ ^) */
} pose_t;

typedef struct { uint16_t ms; pose_t p; } key_t_;     /* hold this pose for `ms`, then go to the next key */
typedef struct { const key_t_ *k; uint8_t n; } seq_t;

#define K(ms, x, y, sw, sh, lL, lR, hL, hR) { ms, { x, y, sw, sh, lL, lR, hL, hR } }
#define SEQ(a) { a, (uint8_t)(sizeof(a) / sizeof(a[0])) }

static const pose_t REST = { 0, 0, 1, 1, 0, 0, 0, 0 };

/* ---- the animations: add one = add an array, a SEQ line in s_anims[] and (optionally) a weight ---- */

static const key_t_ A_BLINK[]   = { K(110, 0, 0, 1, 1, .93f, .93f, 0, 0) };
static const key_t_ A_DBLINK[]  = { K(100, 0, 0, 1, 1, .93f, .93f, 0, 0), K(110, 0, 0, 1, 1, 0, 0, 0, 0),
                                    K(100, 0, 0, 1, 1, .93f, .93f, 0, 0) };
static const key_t_ A_LEFT[]    = { K(1100, -30, 0, .95f, 1, 0, 0, 0, 0) };
static const key_t_ A_RIGHT[]   = { K(1100, 30, 0, .95f, 1, 0, 0, 0, 0) };
static const key_t_ A_UP[]      = { K(1000, 0, -20, 1, 1.05f, 0, 0, 0, 0) };
static const key_t_ A_DOWN[]    = { K(900, 0, 18, 1, .9f, .12f, .12f, 0, 0) };
static const key_t_ A_AROUND[]  = { K(750, -30, -6, .95f, 1, 0, 0, 0, 0), K(220, 0, 0, 1, 1, .93f, .93f, 0, 0),
                                    K(800, 30, -6, .95f, 1, 0, 0, 0, 0), K(500, 0, 10, 1, .95f, 0, 0, 0, 0) };
static const key_t_ A_HAPPY[]   = { K(260, 0, -8, 1.05f, 1, 0, 0, .85f, .85f), K(260, 0, 0, 1.05f, 1, 0, 0, .85f, .85f),
                                    K(260, 0, -8, 1.05f, 1, 0, 0, .85f, .85f), K(260, 0, 0, 1.05f, 1, 0, 0, .85f, .85f),
                                    K(500, 0, -6, 1.05f, 1, 0, 0, .85f, .85f) };
static const key_t_ A_LAUGH[]   = { K(90, -6, -4, 1.05f, 1, 0, 0, .9f, .9f), K(90, 6, -8, 1.05f, 1, 0, 0, .9f, .9f),
                                    K(90, -6, -4, 1.05f, 1, 0, 0, .9f, .9f), K(90, 6, -8, 1.05f, 1, 0, 0, .9f, .9f),
                                    K(90, -6, -4, 1.05f, 1, 0, 0, .9f, .9f), K(90, 6, -8, 1.05f, 1, 0, 0, .9f, .9f),
                                    K(90, -6, -4, 1.05f, 1, 0, 0, .9f, .9f), K(300, 0, -6, 1.05f, 1, 0, 0, .9f, .9f) };
static const key_t_ A_SURPRISE[] = { K(1000, 0, -4, 1.18f, 1.22f, 0, 0, 0, 0) };
static const key_t_ A_SLEEPY[]  = { K(500, 0, 6, 1, .95f, .38f, .38f, 0, 0), K(700, 0, 10, 1, .95f, .62f, .62f, 0, 0),
                                    K(350, 0, 6, 1, .95f, .30f, .30f, 0, 0), K(400, 0, 12, 1, .95f, .75f, .75f, 0, 0),
                                    K(200, 0, 0, 1, 1, 0, 0, 0, 0) };
static const key_t_ A_WINK[]    = { K(600, 0, 0, 1, 1, 0, .93f, .5f, 0) };
static const key_t_ A_SUSPECT[] = { K(1400, 22, 0, .95f, 1, .38f, .5f, 0, 0) };
static const key_t_ A_WAKE[]    = { K(120, 0, 0, 1, 1, .93f, .93f, 0, 0), K(300, 0, 0, 1, 1, .5f, .5f, 0, 0),
                                    K(200, 0, 0, 1, 1, .93f, .93f, 0, 0), K(200, 0, 0, 1.1f, 1.1f, 0, 0, 0, 0) };
/* Falling asleep: droop, nod, eyes shut. The last key is held (see run_hold) so the eyes stay closed. */
static const key_t_ A_SLEEP[]   = { K(450, 0, 6, 1, .95f, .35f, .35f, 0, 0), K(300, 0, 2, 1, 1, .2f, .2f, 0, 0),
                                    K(600, 0, 12, 1, .95f, .93f, .93f, 0, 0) };

typedef struct { seq_t s; uint8_t weight; } anim_t;

static const anim_t s_anims[] = {
    { SEQ(A_BLINK),    30 },
    { SEQ(A_DBLINK),   10 },
    { SEQ(A_LEFT),     10 },
    { SEQ(A_RIGHT),    10 },
    { SEQ(A_UP),        6 },
    { SEQ(A_DOWN),      5 },
    { SEQ(A_AROUND),   10 },
    { SEQ(A_HAPPY),     9 },
    { SEQ(A_LAUGH),     5 },
    { SEQ(A_SURPRISE),  6 },
    { SEQ(A_SLEEPY),    4 },
    { SEQ(A_WINK),      6 },
    { SEQ(A_SUSPECT),   4 },
};
static const seq_t S_WAKE  = SEQ(A_WAKE);
static const seq_t S_SLEEP = SEQ(A_SLEEP);

/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    lv_obj_t *body;
    lv_obj_t *lid;      /* black block over the top part of the eye */
    lv_obj_t *cheek;    /* black circle pushing up from the bottom (happy) */
} eye_t;

static lv_obj_t   *s_scr   = NULL;
static lv_timer_t *s_timer = NULL;
static eye_t       s_eye[2];

/* Animated values. Position and size follow a lightly bouncy spring, lids snap quickly. */
static float s_x, s_y, s_sw, s_sh;
static float s_vx, s_vy, s_vsw, s_vsh;
static float s_lid[2], s_hap[2];
static pose_t s_target;

static const seq_t *s_seq     = NULL;
static uint8_t      s_idx     = 0;
static uint32_t     s_key_end = 0;
static uint32_t     s_next_at = 0;
static bool         s_hold    = false;      /* sleeping: stay on the last key */
static int          s_last    = -1;

static float rnd(void) { return (esp_random() & 0xFFFF) / 65535.0f; }

static void spring(float *cur, float *vel, float target)
{
    *vel = *vel * 0.55f + (target - *cur) * 0.25f;
    *cur += *vel;
}

static float lerp_to(float cur, float target)
{
    return cur + (target - cur) * 0.5f;
}

static void apply_key(const key_t_ *k)
{
    s_target = k->p;
}

static void start_seq(const seq_t *s, uint32_t now)
{
    s_seq = s;
    s_idx = 0;
    apply_key(&s->k[0]);
    s_key_end = now + s->k[0].ms;
}

static const seq_t *pick(void)
{
    int total = 0;
    for (size_t i = 0; i < sizeof(s_anims) / sizeof(s_anims[0]); i++) total += (int)i == s_last ? 0 : s_anims[i].weight;
    int r = (int)(rnd() * total);
    for (size_t i = 0; i < sizeof(s_anims) / sizeof(s_anims[0]); i++) {
        if ((int)i == s_last) continue;                   /* never the same thing twice in a row */
        r -= s_anims[i].weight;
        if (r < 0) { s_last = (int)i; return &s_anims[i].s; }
    }
    s_last = 0;
    return &s_anims[0].s;
}

static void schedule(uint32_t now)
{
    if (s_seq) {
        if ((int32_t)(now - s_key_end) < 0) return;
        if (s_idx + 1 < s_seq->n) {
            s_idx++;
            apply_key(&s_seq->k[s_idx]);
            s_key_end = now + s_seq->k[s_idx].ms;
            return;
        }
        if (s_hold) return;                                /* asleep: stay on the last pose (eyes shut) */
        s_seq = NULL;                                      /* finished: back to the resting face */
        s_target = REST;
        s_next_at = now + 500 + (uint32_t)(rnd() * 1800);  /* a moment of calm before the next move */
        return;
    }
    if (s_hold) return;
    if ((int32_t)(now - s_next_at) >= 0) start_seq(pick(), now);
}

static void layout(uint32_t now)
{
    float breath = 1.0f + 0.018f * sinf(now / 650.0f);
    float w = EYE_W * s_sw;
    float h = EYE_H * s_sh * breath;
    if (w < 8) w = 8;
    if (h < 8) h = 8;

    for (int i = 0; i < 2; i++) {
        eye_t *e = &s_eye[i];
        float cx = SCREEN_SIZE / 2 + (i == 0 ? -1 : 1) * (EYE_W + EYE_GAP) / 2.0f + s_x;
        float cy = SCREEN_SIZE / 2 + s_y;
        int iw = (int)lroundf(w), ih = (int)lroundf(h);

        lv_obj_set_size(e->body, iw, ih);
        lv_obj_set_pos(e->body, (int)lroundf(cx - w / 2), (int)lroundf(cy - h / 2));
        lv_obj_set_style_radius(e->body, (int)(iw * 0.38f), 0);

        float lid = s_lid[i];
        if (lid < 0.02f) {
            lv_obj_add_flag(e->lid, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(e->lid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(e->lid, iw, (int)lroundf(h * lid) + 1);
            lv_obj_set_pos(e->lid, 0, 0);
        }

        float hap = s_hap[i];
        if (hap < 0.02f) {
            lv_obj_add_flag(e->cheek, LV_OBJ_FLAG_HIDDEN);
        } else {
            int d = (int)lroundf(w * 1.25f);
            lv_obj_remove_flag(e->cheek, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(e->cheek, d, d);
            lv_obj_set_pos(e->cheek, (iw - d) / 2, (int)lroundf(h - hap * h * 0.68f));
        }
    }
}

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    schedule(now);

    spring(&s_x, &s_vx, s_target.x);
    spring(&s_y, &s_vy, s_target.y);
    spring(&s_sw, &s_vsw, s_target.sw);
    spring(&s_sh, &s_vsh, s_target.sh);
    s_lid[0] = lerp_to(s_lid[0], s_target.lidL);
    s_lid[1] = lerp_to(s_lid[1], s_target.lidR);
    s_hap[0] = lerp_to(s_hap[0], s_target.hapL);
    s_hap[1] = lerp_to(s_hap[1], s_target.hapR);
    layout(now);
}

static lv_obj_t *black_block(lv_obj_t *parent, bool circle)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, circle ? LV_RADIUS_CIRCLE : 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *pet_init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);          /* the launcher listens for the touch */

    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_obj_create(s_scr);
        lv_obj_set_style_bg_color(b, lv_color_hex(EYE_COLOR), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);       /* the touch goes to the screen */
        s_eye[i].body  = b;
        s_eye[i].lid   = black_block(b, false);
        s_eye[i].cheek = black_block(b, true);
    }

    s_timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
    lv_timer_pause(s_timer);
    pet_start();
    pet_stop();
    return s_scr;
}

void pet_start(void)
{
    s_x = s_y = s_vx = s_vy = s_vsw = s_vsh = 0;
    s_sw = s_sh = 1;
    s_lid[0] = s_lid[1] = 0.93f;                         /* start asleep, then wake up */
    s_hap[0] = s_hap[1] = 0;
    s_target = REST;
    s_hold = false;
    s_last = -1;
    uint32_t now = lv_tick_get();
    start_seq(&S_WAKE, now);
    layout(now);
    lv_timer_resume(s_timer);
}

void pet_stop(void)
{
    if (s_timer) lv_timer_pause(s_timer);
}

void pet_sleep(void)
{
    s_hold = true;
    start_seq(&S_SLEEP, lv_tick_get());
}
