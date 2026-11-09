#include "nav_guide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEAR_M          200     /* "N米后 左转" once the next turn is this close (phone: GUIDE_NEAR_M) */
#define ARRIVED_M       15

static nav_step_t s_steps[NAV_GUIDE_MAX_STEPS];
static int        s_count = 0;
static uint32_t   s_total = 0;

void nav_guide_set_steps(const nav_step_t *steps, int count, uint32_t total_m)
{
    if (!steps || count <= 0) { s_count = 0; return; }
    if (count > NAV_GUIDE_MAX_STEPS) count = NAV_GUIDE_MAX_STEPS;
    memcpy(s_steps, steps, (size_t)count * sizeof(nav_step_t));
    s_count = count;
    s_total = total_m;
}

void nav_guide_clear(void)       { s_count = 0; }
bool nav_guide_has_steps(void)   { return s_count > 0; }

static void copy_str(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src ? src : "");
}

/* Same as the page's numUnit(): >= 1 km in km (one decimal under 10 km), else metres in 10s. */
static void num_unit(uint32_t m, char *num, size_t nn, char *unit, size_t un)
{
    if (m >= 1000) {
        if (m >= 10000) snprintf(num, nn, "%u", (unsigned)((m + 500) / 1000));
        else            snprintf(num, nn, "%u.%u", (unsigned)((m + 50) / 100 / 10), (unsigned)((m + 50) / 100 % 10));
        copy_str(unit, un, "公里");
    } else {
        uint32_t r = (m + 5) / 10 * 10;
        if (r < 10) r = 10;
        snprintf(num, nn, "%u", (unsigned)r);
        copy_str(unit, un, "米");
    }
}

static void append_after(char *unit, size_t un)    /* "米" -> "米后" */
{
    size_t l = strlen(unit);
    if (l + 4 <= un) strcat(unit, "后");
}

bool nav_guide_compute(uint32_t traveled, phone_guide_t *g)
{
    if (!g || s_count <= 0) return false;
    memset(g, 0, sizeof(*g));
    g->has_banner = true;

    /* Next turn: the first one more than 2 m ahead; otherwise the arrival. */
    int mi = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_steps[i].angle != 0 && s_steps[i].dist_m > traveled + 2) { mi = i; break; }
    }
    uint32_t target = (mi >= 0) ? s_steps[mi].dist_m : s_total;
    uint32_t d = (target > traveled) ? (target - traveled) : 0;
    bool near = (d <= NEAR_M);

    char num[12], unit[16];
    num_unit(d, num, sizeof(num), unit, sizeof(unit));

    if (mi < 0) {                                   /* arrival */
        if (d < ARRIVED_M) {
            g->kind = PHONE_GUIDE_ARRIVE;
            copy_str(g->action, sizeof(g->action), "到达目的地");
            return true;
        }
        if (near) {
            g->kind = PHONE_GUIDE_ARRIVE;
            copy_str(g->num, sizeof(g->num), num);
            append_after(unit, sizeof(unit));
            copy_str(g->unit, sizeof(g->unit), unit);
            copy_str(g->action, sizeof(g->action), "到达目的地");
            return true;
        }
    } else if (near) {                              /* a turn is close */
        int a = s_steps[mi].angle;
        int aa = a < 0 ? -a : a;
        const char *side = a < 0 ? "左" : "右";

        g->angle_deg = a;
        g->kind = PHONE_GUIDE_TURN;
        if (aa > 150)       { g->kind = PHONE_GUIDE_UTURN; copy_str(g->action, sizeof(g->action), "掉头"); }
        else if (aa >= 120) snprintf(g->action, sizeof(g->action), "向%s后方转弯", side);
        else if (aa >= 55)  snprintf(g->action, sizeof(g->action), "%s转", side);
        else                snprintf(g->action, sizeof(g->action), "向%s前方行驶", side);

        copy_str(g->num, sizeof(g->num), num);
        append_after(unit, sizeof(unit));
        copy_str(g->unit, sizeof(g->unit), unit);
        if (s_steps[mi].road[0]) {
            copy_str(g->prefix, sizeof(g->prefix), "进入");
            copy_str(g->road, sizeof(g->road), s_steps[mi].road);
        }
        return true;
    }

    /* Far from the next event: "沿 <road> 直行 <distance>". */
    const char *road = "";
    for (int i = 0; i < s_count; i++) {
        if (s_steps[i].dist_m <= traveled) { if (s_steps[i].road[0]) road = s_steps[i].road; }
        else break;
    }
    g->kind = PHONE_GUIDE_STRAIGHT;
    copy_str(g->num, sizeof(g->num), num);
    copy_str(g->unit, sizeof(g->unit), unit);
    copy_str(g->action, sizeof(g->action), "直行");
    copy_str(g->prefix, sizeof(g->prefix), "沿");
    copy_str(g->road, sizeof(g->road), road[0] ? road : "当前道路");
    return true;
}
