#!/usr/bin/env python3
"""
Download Hangzhou Map tiles for Zoom levels 13, 14, 15, and 16,
and convert them directly into LVGL v9 RGB565 .bin format using multithreading.

Tile output structure for microSD card:
  /sdcard/tiles/<z>/<x>/<y>.bin
"""

import os
import math
import time
import io
import zipfile
import requests
from concurrent.futures import ThreadPoolExecutor, as_completed
from PIL import Image

TILE_PX = 256

# Amap / AutoNavi tile endpoint (fast, unblocked in China)
AMAP_URL = "https://webrd02.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=7&x={x}&y={y}&z={z}"
ARCGIS_URL = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}"

USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

# Hangzhou BBox: West Lake, Gongshu, Shangcheng, Binjiang, Xiaoshan, Yuhang
HANGZHOU_BBOX = {
    "lat_min": 30.12,
    "lat_max": 30.38,
    "lon_min": 120.02,
    "lon_max": 120.30
}

def deg2num(lat_deg, lon_deg, zoom):
    lat_rad = math.radians(lat_deg)
    n = 2.0 ** zoom
    xtile = int((lon_deg + 180.0) / 360.0 * n)
    ytile = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return xtile, ytile

def image_to_lvgl_bin(image_bytes):
    im = Image.open(io.BytesIO(image_bytes)).convert("RGB")
    if im.size != (TILE_PX, TILE_PX):
        im = im.resize((TILE_PX, TILE_PX), Image.Resampling.LANCZOS)

    w, h = im.size
    header = bytes([0x19, 0x12]) + (0).to_bytes(2, "little") \
        + w.to_bytes(2, "little") + h.to_bytes(2, "little") \
        + (w * 2).to_bytes(2, "little") + (0).to_bytes(2, "little")

    body = bytearray(w * h * 2)
    px = im.load()
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            body[i] = v & 0xFF
            body[i + 1] = v >> 8
            i += 2

    return header + bytes(body)

def fetch_tile(zoom, x, y):
    headers = {"User-Agent": USER_AGENT}
    for attempt in range(3):
        try:
            url = AMAP_URL.format(z=zoom, x=x, y=y)
            r = requests.get(url, headers=headers, timeout=5)
            if r.status_code == 200 and len(r.content) > 100:
                return r.content
        except Exception:
            pass
        # Fallback to ArcGIS
        try:
            url_arc = ARCGIS_URL.format(z=zoom, x=x, y=y)
            r = requests.get(url_arc, headers=headers, timeout=5)
            if r.status_code == 200 and len(r.content) > 100:
                return r.content
        except Exception:
            pass
        time.sleep(0.1)
    return None

def process_single_tile(item):
    z, x, y, bin_path = item
    if os.path.exists(bin_path) and os.path.getsize(bin_path) == 131084:
        return True, z, x, y, "cached"
    data = fetch_tile(z, x, y)
    if data:
        bin_data = image_to_lvgl_bin(data)
        os.makedirs(os.path.dirname(bin_path), exist_ok=True)
        with open(bin_path, "wb") as f:
            f.write(bin_data)
        return True, z, x, y, "ok"
    return False, z, x, y, "failed"

def download_zoom_range(zooms=[13, 14, 15, 16], output_dir="sdcard_hangzhou", workers=16):
    print(f"=== Downloading Multi-Zoom Hangzhou Tiles ({zooms}) with {workers} workers ===")
    
    tasks = []
    for z in zooms:
        x_min, y_min = deg2num(HANGZHOU_BBOX["lat_max"], HANGZHOU_BBOX["lon_min"], z)
        x_max, y_max = deg2num(HANGZHOU_BBOX["lat_min"], HANGZHOU_BBOX["lon_max"], z)
        total_z = (x_max - x_min + 1) * (y_max - y_min + 1)
        print(f"[Zoom {z}] Grid: x=[{x_min}..{x_max}], y=[{y_min}..{y_max}] -> {total_z} tiles")

        tiles_dir = os.path.join(output_dir, "tiles", str(z))
        for x in range(x_min, x_max + 1):
            x_dir = os.path.join(tiles_dir, str(x))
            for y in range(y_min, y_max + 1):
                bin_path = os.path.join(x_dir, f"{y}.bin")
                tasks.append((z, x, y, bin_path))

    print(f"Total tiles to verify/download: {len(tasks)}")
    
    completed = 0
    cached = 0
    downloaded = 0
    failed = 0
    t0 = time.time()

    with ThreadPoolExecutor(max_workers=workers) as executor:
        future_map = {executor.submit(process_single_tile, item): item for item in tasks}
        for future in as_completed(future_map):
            completed += 1
            ok, z, x, y, status = future.result()
            if ok:
                if status == "cached":
                    cached += 1
                else:
                    downloaded += 1
            else:
                failed += 1

            if completed % 100 == 0 or completed == len(tasks):
                elapsed = time.time() - t0
                speed = downloaded / elapsed if elapsed > 0 else 0
                print(f"Progress: [{completed}/{len(tasks)}] (Cached: {cached}, Downloaded: {downloaded}, Failed: {failed}) @ {speed:.1f} tiles/s")

    print(f"\nDownload summary: {cached} cached, {downloaded} newly fetched, {failed} failed.")

    # Re-package zip
    zip_filename = "hangzhou_sdcard_tiles.zip"
    print(f"\nPackaging '{zip_filename}' for microSD card...")
    with zipfile.ZipFile(zip_filename, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, _, files in os.walk(output_dir):
            for file in files:
                abs_path = os.path.join(root, file)
                rel_path = os.path.relpath(abs_path, output_dir)
                zf.write(abs_path, rel_path)
    
    zip_size_mb = os.path.getsize(zip_filename) / (1024 * 1024)
    print(f"Success! {zip_filename} ({zip_size_mb:.2f} MB) is ready.")

if __name__ == "__main__":
    download_zoom_range(zooms=[13, 14, 15, 16], workers=16)
