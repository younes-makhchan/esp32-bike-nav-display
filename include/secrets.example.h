#pragma once

/* Copy this file to include/secrets.h (git-ignored) and fill in your own values. */

/* The phone's Wi-Fi hotspot: the board joins it so the phone can reach it. */
#define HOTSPOT_SSID     "your-hotspot-name"
#define HOTSPOT_PASSWORD "your-hotspot-password"

/* AMap (高德) "Web端(JS API)" key and its security code, from https://console.amap.com/ .
 * They are put into the companion page at build time (board build and Android app). */
#define AMAP_JS_KEY            "your-amap-web-js-key"
#define AMAP_JS_SECURITY_CODE  "your-amap-security-code"
