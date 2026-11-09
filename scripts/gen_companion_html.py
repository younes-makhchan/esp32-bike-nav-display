"""
Regenerate src/amap_companion_html.h (gzip of src/amap_companion.html as a C array).

Runs automatically before every PlatformIO build (so editing the HTML is enough),
and can also be run by hand:  python3 scripts/gen_companion_html.py
The header is only rewritten when its content would change.
"""
import gzip
import os
import re

try:
    Import("env")  # noqa: F821  (SCons injects this when run as a PlatformIO pre-script)
    PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def secret(name):
    """Value of  #define NAME "..."  in include/secrets.h (git-ignored), or '' when it is not there."""
    try:
        with open(os.path.join(PROJECT_DIR, "include", "secrets.h"), encoding="utf-8") as f:
            m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, f.read())
            return m.group(1) if m else ""
    except OSError:
        return ""


def generate():
    src = os.path.join(PROJECT_DIR, "src", "amap_companion.html")
    dst = os.path.join(PROJECT_DIR, "src", "amap_companion_html.h")

    with open(src, "r", encoding="utf-8") as f:
        html = f.read()
    # The AMap keys are not kept in git: they come from include/secrets.h.
    html = html.replace("__AMAP_KEY__", secret("AMAP_JS_KEY")).replace("__AMAP_SECURITY_CODE__", secret("AMAP_JS_SECURITY_CODE"))
    if not secret("AMAP_JS_KEY"):
        print("[companion] warning: AMAP_JS_KEY is not set in include/secrets.h; the page needs a key (see README)")
    data = gzip.compress(html.encode("utf-8"), compresslevel=9, mtime=0)

    lines = []
    for i in range(0, len(data), 16):
        lines.append(",".join("0x%02x" % b for b in data[i:i + 16]))

    out = (
        "// Auto-generated from src/amap_companion.html by scripts/gen_companion_html.py - do not edit\n"
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        "static const uint32_t AMAP_COMPANION_HTML_GZ_LEN = %d;\n"
        "static const uint8_t AMAP_COMPANION_HTML_GZ[] = {\n%s\n};\n" % (len(data), ",\n".join(lines))
    )

    try:
        with open(dst, "r") as f:
            if f.read() == out:
                return
    except OSError:
        pass

    with open(dst, "w") as f:
        f.write(out)
    print("[companion] regenerated %s (%d bytes gzipped)" % (os.path.basename(dst), len(data)))


generate()
