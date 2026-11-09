import os
import sys

Import("env")

# Ensure lowercase symlinks for Ryzobee headers on Linux case-sensitive filesystems
def ensure_symlinks(*args, **kwargs):
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV/Ryzobee/src/board/rootmaker")
    if not os.path.exists(libdeps_dir):
        return

    symlinks = [
        ("button/Rootmaker_Btn.h", "button/Rootmaker_btn.h"),
        ("lcd/Rootmaker_Lcd.h", "lcd/Rootmaker_lcd.h"),
    ]

    for src_rel, dst_rel in symlinks:
        src = os.path.join(libdeps_dir, src_rel)
        dst = os.path.join(libdeps_dir, dst_rel)
        if os.path.exists(src) and not os.path.exists(dst):
            try:
                os.symlink(os.path.basename(src), dst)
                print(f"[Ryzobee Fix] Created symlink: {dst} -> {src}")
            except Exception as e:
                print(f"[Ryzobee Fix] Failed to create symlink {dst}: {e}")

ensure_symlinks()
env.AddPreAction("checkprogsize", ensure_symlinks)

