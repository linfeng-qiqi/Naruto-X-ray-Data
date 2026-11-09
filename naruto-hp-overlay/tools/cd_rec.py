# -*- coding: utf-8 -*-
"""整条 0x240 属性记录逐字段跟踪：只打印会变化的字段及其序列。
用法：python cd_rec.py <记录基址> [字节数] [秒]
"""
import subprocess, sys, time
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
BASE = int(sys.argv[1], 16)
N = int(sys.argv[2]) if len(sys.argv) > 2 else 0x240
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 14.0
VA = BASE & ~0xFFF
OFF = BASE - VA
SIZE = ((OFF + N + 0xFFF) // 0x1000) << 12


def read(va, n):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null"],
        capture_output=True, timeout=60)
    d = r.stdout
    return d + b"\x00" * (n - len(d)) if len(d) != n else d


fr, tt = [], []
t0 = time.time()
while time.time() - t0 < SECS:
    tt.append(time.time() - t0)
    fr.append(read(VA, SIZE))
    time.sleep(0.05)
b = np.frombuffer(b"".join(fr), dtype=np.uint8).reshape(len(fr), SIZE)
seg = b[:, OFF:OFF + N]
T = np.array(tt)
print(f"{len(fr)} 帧 {len(fr)/SECS:.1f}Hz 记录 {BASE:012X} {N}B")

F = seg.view(np.float32)
I = seg.view(np.int32)
chg = np.nonzero((seg.max(axis=0).astype(np.int16) - seg.min(axis=0).astype(np.int16)) != 0)[0]
# 按字段(4字节对齐)统计变化
field = sorted(set(int(c) // 4 * 4 for c in chg))
print(f"变化字段 {len(field)} / {N//4}")
for f in field:
    vf = F[:, f // 4]
    vi = I[:, f // 4]
    s = " ".join(f"{v:.2f}" if abs(v) < 1e5 else "…" for v in vf[:30])
    si = " ".join(str(v) for v in vi[:12])
    print(f"  +{f:03X} f:[{s}]")
    if not np.all(np.isfinite(vf)) or abs(vf).max() > 1e5:
        print(f"       i:[{si}]")
