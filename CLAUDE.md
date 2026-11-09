# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Despite the repo name, the active firmware is an **AMap (高德) turn-by-turn navigation display** for the **Ryzobee RootMaker** board (ESP32-S3, 16 MB flash, PSRAM, 240×240 touch LCD). A phone opens a companion web page served by the ESP32, which pushes GPS fixes, routes and guidance; the device renders map tiles, route, marker and a turn card with LVGL 9.

**Product goal:** a small dashboard screen that looks and behaves like AMap's own navigation view. You pick a destination on your phone, the phone streams its GPS position and the route, and the device shows the map following you, the route line, and the next turn (arrow, distance, road names) until you arrive. Design decisions (UI style, turn card, tile style, zoom behaviour) should aim to match AMap navigation.

Build system is **PlatformIO, Arduino framework**, single env `rootmaker`.

## Commands

```bash
pio run -e rootmaker                 # build
pio run -e rootmaker -t upload       # flash
pio device monitor -b 115200         # serial monitor (logs go to both USB CDC `Serial` and UART `Serial0`)
```

There are no unit tests (`test/` only has the PlatformIO README).

`scripts/ensure_ryzobee_symlinks.py` runs as a pre-script: it creates lowercase header symlinks inside `.pio/libdeps/rootmaker/Ryzobee/...` because the Ryzobee library has case mismatches that break on Linux. If a build fails on `Rootmaker_btn.h`/`Rootmaker_lcd.h`, re-run the build after libdeps are installed.

## Architecture

Threading model (all FreeRTOS tasks):
- **`lvgl_task`** (core 1, `src/rootmaker_display.cpp`) runs `lv_timer_handler()` under a **recursive mutex**. LVGL is bridged to the Ryzobee LovyanGFX LCD (`pushImageDMA`, byte-swapped RGB565, two 40-line partial DMA buffers) and its touch controller. `LV_USE_OS` is `NONE`, so **every LVGL call from any other task must be wrapped in `rootmaker_display_lock()` / `rootmaker_display_unlock()`** — see `main.cpp` callbacks and `loop()`.
- **`wifi_server`** (core 0, `src/phone_gps.cpp`) runs WiFi AP+STA and an Arduino `WebServer`:
  - AP `ESP32-Hangzhou-Map` at 192.168.4.1; STA credentials (`HOTSPOT_SSID`/`HOTSPOT_PASSWORD`) come from the git-ignored `include/secrets.h` (template `include/secrets.example.h`).
  - `GET /` serves the gzipped companion page from `src/amap_companion_html.h`.
  - `POST /api/gps` and `POST /api/route` (JSON) parse into `phone_gps_fix_t` / `phone_route_t` and invoke callbacks registered by `main.cpp`, which forward them into `map_manager_*` under the display lock.
- **Tile loader task** (`src/map_tiles/map_tile_cache.c`): LRU cache of 256×256 RGB565 tiles. On a miss it reads `/sdcard/amap_cache/z_x_y.bin` if `bsp_sdcard` is set (currently always `NULL` in the Arduino build, so SD is unused), otherwise downloads the PNG live from the AMap tile server over the **STA** connection (`phone_gps_is_wifi_connected()`), decodes and fills the slot. No STA internet → blank tiles.

Layers:
- `src/main.cpp` — board init (`Ryzobee board(RYZOBEE_ROOTMAKER)`), wiring, physical button (short press cycles zoom 15–20, long press re-centers on GPS).
- `src/launcher.cpp` — home screen of app icons (`s_apps[]`; add an app with one line), the "scan to connect" QR screen, and screen switching. The map lives on its own LVGL screen; its overlay widgets sit on the shared top layer, so `map_manager_set_visible()` must be called when switching away from / back to the map. Navigation flow: icon → QR of `http://<device IP>/` (the device joins the phone's hotspot as STA) → phone opens the page → any call from the app (`/api/ping`, `/api/gps`, `/api/route`...) makes `phone_gps_recent_contact()` true (or a fix makes `phone_gps_is_active()` true) → map opens, even before the phone has a GPS fix. Long-press the home icon on the map to return. The home screen also shows the battery (icon + percent, top left; `board.rootmaker.bat`, refreshed every 10 s and when the screen opens; green/amber(≤40 %)/red(≤20 %), a bolt while charging, hidden if the power chip does not answer). The phone stays the GPS source (hotspot, Android); the page starts GPS on load, not only at 开始骑行.
- `src/brightness.cpp` — screen brightness 10–100 %, saved in flash (Preferences key `bri`), applied at boot in `main.cpp`; the launcher's second app (sun icon) opens a slider screen with − / + buttons. The map's home button needs a **long press** (a tap only shows a hint), so a bump cannot end the view.
- `src/rider.cpp` (+ `rider_sprite.inc`) — pixel-art scene (rider next to a yellow mountain bike, wheels turn, battery on top) used as the **idle screen**: the home screen untouched for 3 s shows it, 10 s more without a touch blacks the screen out, any touch wakes it. (`pet.cpp`, the old eyes screen, is no longer used.)
- **Simulation**: the page's green 模拟 button (in the route preview) rides the route by itself (`GPS.simulate`); fixes carry `"sim":true`, the board then ignores real GPS (`phone_gps_sim_active`), and a long press on the map's home button stops it (`phone_gps_sim_stop`; the next `/api/gps` reply says `"stop":true` and the page ends the ride). In the Android app the page relays simulated fixes through `Android.simFix`.
- **Heading-up map**: while a route is active `map_manager.cpp` turns the map (`rot_timer_cb`, 100 ms) so the road ahead (`map_view_route_bearing_ahead`, 70 px lookahead; GPS course above 4 km/h when off route) points up; no route = north-up.
- `src/map_manager.cpp` — the app UI: owns the `map_view` widget, route handle, turn-instruction card, status text, Chinese fonts (`font_chinese_16/20.c`, generated with `lv_font_conv`; see `generate_font.sh`). Zoom range 11–20, default center Hangzhou. The top turn card exists only while a route exists (`card_show`; idle = hidden). Turn arrows are recorded as shapes and scaled so their ink fills the icon height (`shape_draw`), the icon height itself being measured from the fonts (`measure_text_block`), so straight/left/right/U-turn/arrive arrows are all as tall as the two text rows.
- `src/map_tiles/` — vendored "Map Tiles" component (see its `README.md`). The project uses the **v2 navigation API** (`map_view`, `map_route`, `map_tile_cache`, `map_geo`); `map_tiles.h`/`map_tiles.cpp` is the older v1 grid API. It still carries ESP-IDF `CMakeLists.txt`/`idf_component.yml` but is compiled by PlatformIO as part of `src/` (include path `-I src/map_tiles/include`).

## Editing gotchas

- **Companion web page**: edit `src/amap_companion.html` only. `scripts/gen_companion_html.py` (a PlatformIO pre-script, also runnable by hand) regenerates the gzipped `src/amap_companion_html.h` (git-ignored) on every build; never edit that header. The page holds the placeholders `__AMAP_KEY__` / `__AMAP_SECURITY_CODE__`; the script and the Android Gradle task `syncPage` fill them from `AMAP_JS_KEY` / `AMAP_JS_SECURITY_CODE` in `include/secrets.h`. Never put real keys or passwords in tracked files.
- **Phone ↔ device protocol**: `POST /api/gps` with `lat, lon, heading, speed(km/h), nav` (+ `guide`/`info` while navigating, the phone's own banner). `POST /api/route` is sent **only when the rider presses 开始骑行** (and on a mid-ride re-route) and carries `waypoints`, `traffic_lights` and `st` — steps `[lng, lat, angle, road]` (angle 0 = road rename only). `POST /api/clear` on ride end / page load. `GET /api/ping` → `bike-nav` (the Termux feeder uses it to find the device).
- **Banner is computed on the device** (`src/nav_guide.cpp`) whenever the route came with steps: the device matches the position to the route and builds the same text as the page's `guideForWatch()`. Keep the two in sync (they were checked identical on 40 cases). Without steps the device falls back to mirroring the phone's `guide`.
- **Background GPS (screen off), older path**: `termux/bike_gps.py` (superseded by the Android app) runs in Termux (+ Termux:API), reads GPS with a wake lock and posts raw WGS-84 fixes (`"wgs":true,"src":"t"`) to `/api/gps`; the device converts to GCJ-02 (`src/coord_cn.cpp`). While such fixes are arriving (5 s), the page's own `/api/gps` posts are ignored so the two sources do not fight. The page's JS is frozen with the screen off, so nothing may depend on it at ride time. Arrow direction from the feeder is the GPS course (only at 4 km/h or more); `--compass` (opt-in, phone mounted facing forward) adds the rotation-vector compass for when stopped, calibrated against the GPS course while riding.
- **Android app** (`android/`, plain Kotlin; only dependency is ZXing core for the QR scanner; ~1.9 MB): the phone-side app. It shows **the same page** (`src/amap_companion.html`, copied into the app's assets by the Gradle task `syncPage` — one source file) in a full-screen WebView, and the page detects it with `window.Android` (`NATIVE` in the page; every app-only branch is guarded, so the page still works when served by the board). Division of labour in app mode: the page does the UI, AMap JS search, `AMap.Riding` planning and preview; the app does the rest. `GpsService` (foreground service + wake lock, plain `LocationManager`, optional compass) streams GPS to the unit (`/api/gps`, same message as `bike_gps.py`) and pushes each fix into the page (`nativeFix(lng,lat,heading,kmh)`, converted to GCJ-02 by `Geo`); the page's "start riding" calls `Android.startRide(json)` → `routeFromPageJson` → `Route.toDeviceJson()` → `POST /api/route`, and `Android.endRide()` → `/api/clear`. The service follows the ridden route (`RouteMatcher`: <25 m from the destination = arrived → clear; 3 fixes >60 m = off route → re-plans via AMap REST `Amap.bicycling` **only if** an optional Web服务 key is set and the page is not on screen). The QR scanner (`ScanActivity` + `ViewfinderView` + `ScanGeometry`) shows an undistorted preview with a framed square, decodes only the framed centre of the camera image (every 5th miss the whole image, two binarizers), re-focuses every 2.5 s, vibrates and turns green on success, and has "Type the IP instead". First run shows a checklist (bike unit via `ScanActivity` QR scan or typed IP, location, battery/background); the gear next to the search bar opens native Settings (AMap Web端 key + security code override the built-in ones; optional Web服务 key; unit IP; compass; start/stop GPS). Location sources: GPS (fixes worse than 100 m ignored) plus the network position as a fallback while no GPS fix arrived in the last 15 s, and the last known position at start; the gear's Settings dialog has a Status block (service, last position + source + age, unit address + last send result, log) — look there first when "nothing happens". Compass: on by default (Settings); the rotation-vector sensor (or accelerometer+magnetometer) is read at UI rate, corrected to true north (`GeomagneticField`), and pushed to the page (`nativeHeading`) and the unit (same position, new heading, at most 4/s) whenever the phone turns and the rider is not moving — independent of GPS fixes; at 4 km/h or more the arrow is locked to the GPS course (the way the road runs) and the compass only calibrates against it (above 9 km/h); below 4 km/h the compass steers again (`Heading.MIN_SPEED_FOR_HEADING`). The page shows a direction beam on the blue dot in browse mode (`has-heading`) and the arrow + ring while riding. Tests: `cd android && ANDROID_HOME=$HOME/android-sdk ./gradlew testDebugUnitTest` (16, incl. a real route captured from the page: `src/test/resources/page_route_sample.json`). Build: `./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`; install with `adb install -r`. The SDK is in `~/android-sdk` (outside the repo); `android/local.properties` points to it (git-ignored). To try the page's app mode on a computer, inject a fake `window.Android` and open it in headless Chrome (the way it was verified). Firmware needs no change; the board still serves the page as a fallback.
- `lv_conf.h` exists in both `src/` and `include/` (currently identical); keep them in sync, since both dirs are on the include path.
- Large allocations (tile slots, route polylines) go to PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM ...)`; LVGL draw buffers must stay in internal DMA RAM.
- `lib/` is **gitignored** (vendored local libs such as `Yoke_Voice` for the CI1302 voice chip, ES8311, SensorLib, etc.); changes there won't be committed.

## Non-firmware / legacy content

- `src_backup_idf/` — previous ESP-IDF version of the app (root `CMakeLists.txt`, `sdkconfig*`, `dependencies.lock`, `managed_components/` belong to that era). Not built by PlatformIO.
- `scripts/download_hangzhou_map.py` — offline tool that downloads Hangzhou tiles (AMap, ArcGIS fallback) and converts them to LVGL v9 RGB565 `.bin` for an SD card.
- `capture_mic.py` — host-side tool (pyserial) to record/decode CI1302 microphone audio over serial using the "cias-v1" frame format (`--record`, `--decode`, etc.; see its docstring). It pairs with yoke-voice firmware, not the current `main.cpp`.
- `ESP32-S3-Touch-AMOLED-2.06/`, `xiaozhi-esp32/`, `demo/`, `amapDemo/`, `web_prototype/`, `termux_companion/`, `livetranslatedemo/` are demos only — not part of the product. Use them for reference, don't build on them.

Before coding:
1. Identify the files that need changing.
2. Explain your implementation plan.
3. Check for compatibility with existing code.

Then implement the feature.

After implementation:
1. Run pio run.
2. Fix compilation errors.
3. Summarize all modified files.
4. Explain how to test the feature on hardware.