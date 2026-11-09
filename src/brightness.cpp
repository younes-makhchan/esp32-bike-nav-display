#include "brightness.h"

#include <Arduino.h>
#include <Preferences.h>
#include "rootmaker_display.h"      // extern Ryzobee board

static int clamp_pct(int pct)
{
    if (pct < BRIGHTNESS_MIN_PCT) return BRIGHTNESS_MIN_PCT;
    if (pct > 100) return 100;
    return pct;
}

int brightness_load(void)
{
    Preferences p;
    int pct = 100;
    if (p.begin("bike", true)) {            // read-only
        pct = p.getInt("bri", 100);
        p.end();
    }
    return clamp_pct(pct);
}

void brightness_set(int pct, bool save)
{
    pct = clamp_pct(pct);
    if (board.rootmaker.lcd) {
        board.rootmaker.lcd->setBrightness((uint8_t)((pct * 255 + 50) / 100));
    }
    if (save) {
        Preferences p;
        if (p.begin("bike", false)) {
            p.putInt("bri", pct);
            p.end();
        }
    }
}

void brightness_off(void)
{
    if (board.rootmaker.lcd) board.rootmaker.lcd->setBrightness(0);
}
