# ESP32 bike navigation display

Turn-by-turn bike navigation on a small **240×240 touch display** driven by an **ESP32-S3**
([Ryzobee RootMaker](https://www.ryzobee.com/) board). You pick a destination on your phone; the phone streams
its GPS position and the route to the board, and the board shows an AMap (高德) style navigation view:
the map following you and turning so the road ahead is always at the top, the route line, and the next turn
(arrow, distance, road names) until you arrive.

The board draws everything itself with LVGL 9. The phone does the heavy lifting (search, route planning, GPS).

## What is in here

| Part | Where | What it does |
|---|---|---|
| Firmware | `src/`, `platformio.ini` | PlatformIO / Arduino project for the RootMaker (ESP32-S3, 16 MB flash, PSRAM). Launcher, map, turn card, brightness screen and an idle screen with a pixel-art rider. |
| Companion page | `src/amap_companion.html` | The phone UI: destination search, AMap cycling route preview, start / simulate buttons. Served by the board as a fallback and bundled into the Android app. |
| Android app | `android/` | Plain Kotlin app: shows the page full screen, keeps streaming GPS in the background (screen off), sends the route to the board, optional compass. |
| Map tiles | `src/map_tiles/` | Vendored map-tile component (tile cache, map view, routes). |
| Termux feeder | `termux/` | Older background-GPS script, superseded by the Android app. |

## How it works

```
phone (Android app + web page)  --Wi-Fi hotspot-->  ESP32-S3 board
  GPS, AMap search/route                              POST /api/gps    position, heading, speed
  "start riding" / "simulate"                         POST /api/route  route + turn steps
                                                      POST /api/clear  ride ended
```

- The board joins the phone's hotspot. The home screen shows a QR code of the board's address; the phone opens
  it and the map appears as soon as the phone connects.
- Map tiles are downloaded by the board from the AMap tile server over the same hotspot.
- The turn banner is computed on the board from the route, so it keeps working with the phone screen off.
- **Simulation:** after choosing a destination, the green *模拟* button rides the route by itself (the arrow
  moves on the phone and on the board). Stop it with ✕ on the phone, or long-press the home button on the board.

## Setup

1. **Secrets.** Copy `include/secrets.example.h` to `include/secrets.h` (git-ignored) and fill in:
   - `HOTSPOT_SSID` / `HOTSPOT_PASSWORD`: your phone's hotspot.
   - `AMAP_JS_KEY` / `AMAP_JS_SECURITY_CODE`: an AMap *Web端 (JS API)* key and its security code from
     <https://console.amap.com/>. They are inserted into the page at build time and are never committed.
2. **Firmware** ([PlatformIO](https://platformio.org/)):
   ```bash
   pio run -e rootmaker                 # build
   pio run -e rootmaker -t upload       # flash
   pio device monitor -b 115200         # serial log
   ```
   If the build fails on `Rootmaker_btn.h` / `Rootmaker_lcd.h`, run it once more after the libraries are installed
   (`scripts/ensure_ryzobee_symlinks.py` fixes case mismatches in the Ryzobee library on Linux).
3. **Android app** (Android SDK required, path in `android/local.properties`):
   ```bash
   cd android
   ./gradlew assembleDebug              # app/build/outputs/apk/debug/app-debug.apk
   ./gradlew testDebugUnitTest          # unit tests
   adb install -r app/build/outputs/apk/debug/app-debug.apk
   ```
   On first start the app asks for the board's address (scan the QR code on the board or type the IP), the
   location permission and background-battery permission.

## Use

1. Switch on the phone hotspot and the board; the board joins it.
2. Tap the navigation icon on the board and scan the QR code with the app.
3. Search a destination in the app, then *开始骑行* (start) or *模拟* (simulate).
4. Buttons on the board: short press = zoom, long press = re-centre; the home button on the map needs a long press.

## Notes

- Map data is AMap (GCJ-02 coordinates); it covers China only.
- The default map centre is Hangzhou.
- AMap tiles and APIs have their own terms of use; use your own key.

## License

[MIT](LICENSE). The vendored component in `src/map_tiles/` keeps its own licence (`src/map_tiles/LICENSE`).
