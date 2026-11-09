#!/usr/bin/env python3
"""
bike_gps.py - keep sending the phone's GPS to the bike unit while the phone screen is OFF.

Runs in Termux (with the Termux:API app). Web pages stop reading GPS when the screen locks;
this does not, because Termux holds a wake lock and asks Android for the location itself.

One-time setup (install Termux AND Termux:API from the SAME source: F-Droid or GitHub):
    pkg install python termux-api
    termux-location            # accept the Android location permission once
    (Android settings -> Apps -> Termux and Termux:API -> Battery -> Unrestricted)

Every ride:
    1. Turn on the hotspot; the bike unit joins it. Plan the route in the page as usual
       and press 开始骑行.
    2. In Termux:   python bike_gps.py        (or: python bike_gps.py 192.168.43.123)
    3. Lock the phone and ride. Ctrl+C (or close Termux) stops it.

Direction of the arrow: by default the GPS course (the way you are moving; only while above
~3.6 km/h, otherwise the last direction is kept). Add  --compass  ONLY if the phone is mounted
facing the way the bike points (e.g. on the handlebar): the compass then turns the arrow while
you are stopped too. It teaches itself the mount's small misalignment from the GPS course
while you ride. In a pocket or bag the compass would point the wrong way - leave it off.

What it sends: only GPS (latitude, longitude, speed, direction) to the bike unit, over the
hotspot. The unit converts it to AMap coordinates and works out the turn banner itself.
"""

import atexit
import fcntl
import json
import math
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

CACHE = os.path.expanduser("~/.bike_ip")
MAX_ACCURACY_M = 50          # ignore fixes worse than this
MIN_SPEED_FOR_HEADING = 4.0 / 3.6  # m/s (4 km/h); below this the GPS "bearing" is noise, so do not send one
RESCAN_AFTER_FAILURES = 3
COMPASS_MAX_AGE = 2.0        # s; an older compass reading is treated as "no compass"
LEARN_SPEED = 2.5            # m/s (9 km/h): above this, GPS course is trusted to calibrate the compass


def log(msg):
    print(time.strftime("%H:%M:%S "), msg, flush=True)


# ------------------------------------------------------------ finding the device

def is_bike(host):
    try:
        with urllib.request.urlopen("http://%s/api/ping" % host, timeout=0.6) as r:
            return r.read(16) == b"bike-nav"
    except Exception:
        return False


def local_subnets():
    """The /24 networks of this phone's interfaces (the hotspot is one of them)."""
    nets = []
    try:
        names = [n for _, n in socket.if_nameindex()]
    except OSError:
        names = []
    for name in names:
        if name == "lo" or name.startswith(("rmnet", "dummy", "tun", "ccmni", "ifb")):
            continue
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            raw = fcntl.ioctl(s.fileno(), 0x8915, struct.pack("256s", name[:15].encode()))
            s.close()
            nets.append(socket.inet_ntoa(raw[20:24]).rsplit(".", 1)[0])
        except OSError:
            continue
    nets.append("192.168.43")        # the classic Android hotspot subnet
    return list(dict.fromkeys(nets))


def scan():
    hosts = ["%s.%d" % (net, i) for net in local_subnets() for i in range(2, 255)]
    with ThreadPoolExecutor(max_workers=48) as pool:
        for host, ok in zip(hosts, pool.map(is_bike, hosts)):
            if ok:
                return host
    return None


def find_device(explicit=None):
    if explicit:
        return explicit
    cached = None
    try:
        cached = open(CACHE).read().strip()
    except OSError:
        pass
    if cached and is_bike(cached):
        return cached
    log("looking for the bike unit on the hotspot ...")
    host = scan()
    if host:
        try:
            open(CACHE, "w").write(host)
        except OSError:
            pass
    return host


# ----------------------------------------------------------------------- heading

def angle_diff(a, b):
    """Signed difference a - b in degrees, in -180..180."""
    return (a - b + 180.0) % 360.0 - 180.0


def heading_from_quaternion(x, y, z, w):
    """
    Compass heading (degrees clockwise from north) of the way the phone FACES, from Android's
    rotation-vector quaternion. Lying flat, that is the top edge; upright in a mount (screen
    toward the rider), it is the back of the phone.
    """
    return _heading_from_matrix(2 * (x * y - z * w), 1 - 2 * (x * x + z * z),
                                2 * (x * z + y * w), 2 * (y * z - x * w),
                                1 - 2 * (x * x + y * y))


def _heading_from_matrix(r01, r11, r02, r12, r22):
    """R maps phone axes to (east, north, up). Flat: the top edge (y axis) is the front; else the back (-z)."""
    if abs(r22) > 0.75:
        east, north = r01, r11
    else:
        east, north = -r02, -r12
    return math.degrees(math.atan2(east, north)) % 360.0


def heading_from_accel_mag(a, m):
    """
    Same heading from the two sensors EVERY phone has: accelerometer (gravity, pointing up
    when at rest) and magnetometer, both in phone axes. This is what a browser's compass does.
    """
    ax, ay, az = a
    ex, ey, ez = m
    hx, hy, hz = ey * az - ez * ay, ez * ax - ex * az, ex * ay - ey * ax     # east = E x A
    nh = math.sqrt(hx * hx + hy * hy + hz * hz)
    na = math.sqrt(ax * ax + ay * ay + az * az)
    if nh < 0.1 or na < 0.1:
        return None                                                          # free fall / no field
    hx, hy, hz = hx / nh, hy / nh, hz / nh
    ax, ay, az = ax / na, ay / na, az / na
    mx, my, mz = ay * hz - az * hy, az * hx - ax * hz, ax * hy - ay * hx     # north = A x H
    return _heading_from_matrix(hy, my, hz, mz, az)


class Heading:
    """GPS course while moving; the compass (aligned to the last GPS course) when stopped."""

    def __init__(self):
        self.offset = None                    # compass -> GPS correction, learned while riding

    def pick(self, speed, bearing, compass):
        """-> (degrees, source) or (None, None)"""
        if compass is not None and bearing is not None and speed >= LEARN_SPEED:
            if self.offset is None:
                self.offset = (bearing - compass) % 360.0
            else:
                self.offset = (self.offset + 0.2 * angle_diff(bearing, compass + self.offset)) % 360.0
        if bearing is not None and speed >= MIN_SPEED_FOR_HEADING:
            return bearing % 360.0, "gps"
        if compass is not None:
            return (compass + (self.offset or 0.0)) % 360.0, "compass"
        return None, None


class Compass(threading.Thread):
    """
    Streams phone sensors via termux-sensor and keeps the latest heading.
    names = [rotation-vector]  or  [accelerometer, magnetometer]
    """

    def __init__(self, names):
        super().__init__(daemon=True)
        self.names = names
        self.latest = {}
        self.heading = None
        self.ts = 0.0
        self.proc = None

    def current(self):
        if self.heading is not None and time.time() - self.ts < COMPASS_MAX_AGE:
            return self.heading
        return None

    def stop(self):
        if self.proc:
            self.proc.terminate()

    def _handle(self, obj):
        for name, v in obj.items():
            vals = v.get("values") if isinstance(v, dict) else None
            if vals and len(vals) >= 3:
                self.latest[name] = vals
        h = None
        if len(self.names) == 1:
            vals = self.latest.get(self.names[0])
            if vals:
                x, y, z = vals[0], vals[1], vals[2]
                w = vals[3] if len(vals) > 3 else math.sqrt(max(0.0, 1 - x * x - y * y - z * z))
                h = heading_from_quaternion(x, y, z, w)
        else:
            acc, mag = self.latest.get(self.names[0]), self.latest.get(self.names[1])
            if acc and mag:
                h = heading_from_accel_mag(acc[:3], mag[:3])
        if h is not None:
            self.heading = h
            self.ts = time.time()

    def run(self):
        self.proc = subprocess.Popen(["termux-sensor", "-s", ",".join(self.names), "-d", "250"],
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        dec = json.JSONDecoder()
        buf = ""
        fd = self.proc.stdout.fileno()
        while True:
            data = os.read(fd, 4096)
            if not data:
                return
            buf += data.decode("utf-8", "ignore")
            while True:                       # the sensor prints pretty JSON objects back to back
                buf = buf.lstrip()
                if not buf:
                    break
                try:
                    obj, end = dec.raw_decode(buf)
                except ValueError:
                    break                     # incomplete: wait for more
                buf = buf[end:]
                self._handle(obj)


def list_sensors():
    """-> (names, error). Names are exactly what `termux-sensor -l` prints."""
    try:
        r = subprocess.run(["termux-sensor", "-l"], capture_output=True, text=True, timeout=15)
    except subprocess.TimeoutExpired:
        return [], "termux-sensor -l timed out (is the Termux:API app installed and allowed to run?)"
    except FileNotFoundError:
        return [], "termux-sensor not found (pkg install termux-api)"
    try:
        return json.loads(r.stdout).get("sensors", []), None
    except ValueError:
        return [], "termux-sensor -l gave: %r %r" % (r.stdout[:120], r.stderr[:120])


def pick_compass_sensors(names):
    """
    -> [rotation-vector] if the phone has an absolute one (not the "game" one: it has no north),
       else [accelerometer, magnetometer], else None.
    """
    norm = [(n, n.lower().replace("_", " ")) for n in names]

    def first(ok):
        for name, low in norm:
            if ok(low):
                return name
        return None

    rot = (first(lambda l: "rotation vector" in l and "game" not in l and "geomagnetic" not in l)
           or first(lambda l: "geomagnetic" in l and "rotation" in l))
    if rot:
        return [rot]
    bad = ("uncalib", "linear", "gravity", "limited", "wake", "gyro")
    acc = first(lambda l: "accelerometer" in l and not any(b in l for b in bad))
    mag = first(lambda l: ("magnetic" in l or "magnetometer" in l) and not any(b in l for b in bad))
    return [acc, mag] if acc and mag else None


# --------------------------------------------------------------------------- GPS

def read_fix():
    try:
        out = subprocess.run(["termux-location", "-p", "gps", "-r", "once"],
                             capture_output=True, text=True, timeout=12).stdout
    except subprocess.TimeoutExpired:
        return None
    except FileNotFoundError:
        sys.exit("termux-location not found: run  pkg install termux-api  and install the Termux:API app")
    try:
        return json.loads(out) if out.strip() else None
    except ValueError:
        return None


def build_payload(fix, mixer=None, compass=None):
    """Termux fix -> (/api/gps body, direction source) or (None, None) if the fix is unusable."""
    if not fix or "latitude" not in fix or "longitude" not in fix:
        return None, None
    acc = fix.get("accuracy")
    if acc is not None and acc > MAX_ACCURACY_M:
        return None, None
    speed = float(fix.get("speed") or 0.0)                       # m/s
    payload = {
        "lat": fix["latitude"],
        "lon": fix["longitude"],
        "wgs": True,                                             # raw GPS: the unit converts it for AMap
        "src": "t",                                              # tells the unit this is the background feeder
        "speed": round(speed * 3.6, 1),                          # km/h
    }
    bearing = fix.get("bearing")
    bearing = float(bearing) if bearing is not None else None
    heading, source = (mixer or Heading()).pick(speed, bearing, compass)
    if heading is not None:
        payload["heading"] = round(heading) % 360
    return payload, source


def post(host, payload):
    req = urllib.request.Request("http://%s/api/gps" % host, data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=2) as r:
        return r.status == 200


# -------------------------------------------------------------------------- main

def main(argv):
    explicit = None
    limit = None                      # --count N: stop after N posts (testing)
    use_compass = False
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--count" and args:
            limit = int(args.pop(0))
        elif a == "--compass":
            use_compass = True
        else:
            explicit = a

    subprocess.run(["termux-wake-lock"], capture_output=True)    # keep running with the screen off
    atexit.register(lambda: subprocess.run(["termux-wake-unlock"], capture_output=True))

    compass = None
    if use_compass:
        names, err = list_sensors()
        chosen = pick_compass_sensors(names)
        if chosen:
            compass = Compass(chosen)
            compass.start()
            atexit.register(compass.stop)
            log("compass on (%s): the phone must face the way the bike points" % " + ".join(chosen))
        else:
            log("no usable compass sensor - using the GPS direction only")
            log(err or ("sensors on this phone: " + ", ".join(names)))
    mixer = Heading()

    host = find_device(explicit)
    while not host:
        log("bike unit not found - is the hotspot on and the unit connected? retrying ...")
        time.sleep(3)
        host = find_device(explicit)
    log("bike unit at %s" % host)

    sent = failures = 0
    while limit is None or sent < limit:
        payload, source = build_payload(read_fix(), mixer, compass.current() if compass else None)
        if not payload:
            time.sleep(0.3)
            continue
        try:
            post(host, payload)
            sent += 1
            failures = 0
            log("%.5f,%.5f  %s km/h  %s" % (payload["lat"], payload["lon"], payload["speed"],
                "%d deg (%s)" % (payload["heading"], source) if "heading" in payload else "no direction"))
        except Exception as e:
            failures += 1
            log("send failed (%s)" % e)
            if failures >= RESCAN_AFTER_FAILURES and not explicit:
                host = find_device() or host
                failures = 0
            time.sleep(1)


if __name__ == "__main__":
    try:
        main(sys.argv)
    except KeyboardInterrupt:
        log("stopped")
