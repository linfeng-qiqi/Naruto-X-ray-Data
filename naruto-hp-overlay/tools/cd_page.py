# -*- coding: utf-8 -*-
"""按页高频观察：在指定页附近找 10->0 连续倒计时（真正的时间字段）
用法：python cd_page.py <中心VA> [页数] [秒]
"""
import subprocess, sys, time
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PKG = "com.tencent.KiHan"
PID = subprocess.run([ADB, "shell", "pidof", PKG], capture_output=True, text=True).stdout.strip()
CENTER = int(sys.argv[1], 16)
NP = int(sys.argv[2]) if len(sys.argv) > 2 else 3
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 26.0
VA = ((CENTER >> 12) - NP) << 12
SIZE = (2 * NP + 1) << 12
print(f"pid={PID} 页区 {VA:012X} {SIZE//1024}KB", flush=True)


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
    fr.append(np.frombuffer(read(VA, SIZE), dtype=np.uint8).copy())
A = np.frombuffer(b"".join(fr), dtype=np.uint8).reshape(len(fr), SIZE).view(np.float32)
T = np.array(tt)
print(f"{len(fr)} 帧, {len(fr)/SECS:.1f}Hz", flush=True)

mn = A.min(axis=0); mx = A.max(axis=0)
cand = np.nonzero(np.isfinite(mn) & (mn >= -0.5) & (mn <= 1.6) & (mx >= 7.6) & (mx <= 11.0))[0]
print(f"候选 offset {len(cand)}", flush=True)
hits = []
for c in cand:
    s = A[:, c].astype(np.float64)
    if not np.all(np.isfinite(s)): continue
    d = np.diff(s)
    # 找最长连续递减段，斜率 -1
    best = 0.0; i = 0
    while i < len(s) - 1:
        j = i
        while j + 1 < len(s):
            dt = T[j+1] - T[j]
            if dt <= 0: break
            if -1.5 < (s[j+1] - s[j]) / dt < -0.5: j += 1
            else: break
        best = max(best, T[j] - T[i]); i = max(j, i + 1)
    if best >= 5.0:
        hits.append((best, int(c), s))
hits.sort(key=lambda r: -r[0])
print(f"\n=== 连续倒计时候选 {len(hits)} 个 ===", flush=True)
for best, c, s in hits[:10]:
    print(f"  {VA + c*4:012X} 递减 {best:.1f}s", flush=True)
    print("     ", " ".join(f"{v:.2f}" for v in s[:44]), flush=True)
# 顺带打印中心地址前后 0x80 的浮点布局
c0 = (CENTER - VA) // 4
print(f"\n-- 中心 {CENTER:012X} 附近浮点（每 4 字节）--", flush=True)
for k in range(c0 - 24, c0 + 25, 4):
    va = VA + k * 4
    print(f"  {va:012X} " + " ".join(f"{A[-1, k+m]:>11.3f}" for m in range(4)), flush=True)
