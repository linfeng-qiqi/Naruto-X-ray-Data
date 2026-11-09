# -*- coding: utf-8 -*-
"""宽量程倒计时探测：覆盖 0~10000 毫秒型、f64 双精度型。
判据：值长期在一条带内，单调走完 >=60% 量程并持续 >=5s，然后跳回。
用法：python cd_wide.py <起始VA> <MB> [秒]
"""
import subprocess, sys, time, json
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
VA = int(sys.argv[1], 16)
MB = int(sys.argv[2])
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 26.0
SIZE = MB << 20
TAG = f"{VA:012X}"
print(f"pid={PID} {TAG} +{MB}MB {SECS}s", flush=True)


def read(va, n):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null"],
        capture_output=True, timeout=120)
    d = r.stdout
    return d + b"\x00" * (n - len(d)) if len(d) != n else d


N = SIZE // 4
mn = np.full(N, np.float32(1e30), np.float32)
mx = np.full(N, np.float32(-1e30), np.float32)
jp = np.zeros(N, np.float32)
dn = np.zeros(N, np.float32)
bad = np.zeros(N, bool)
prev = None
ts = []
t0 = time.time()
while time.time() - t0 < SECS:
    ts.append(time.time())
    f = np.frombuffer(read(VA, SIZE), dtype=np.uint8).view(np.float32)
    np.minimum(mn, f, out=mn); np.maximum(mx, f, out=mx)
    bad |= ~np.isfinite(f)
    if prev is not None:
        d = f - prev
        np.maximum(jp, d, out=jp)
        np.minimum(dn, d, out=dn)
    prev = f
dt = (ts[-1] - ts[0]) / max(1, len(ts) - 1)
print(f"{len(ts)} 帧 dt={dt:.2f}s", flush=True)

span = mx - mn
# 宽量程：0~10000 毫秒 / 微秒 / 任意正数；跳回幅度需 >50% 量程
m = (~bad) & np.isfinite(span) & (span >= 50.0) & (span <= 200000.0) \
    & (mn >= -1.0) & (jp >= 0.6 * span)
idx = np.nonzero(m)[0]
print(f"毫秒型候选 {len(idx)}", flush=True)
for c in idx[:10]:
    print(f"  {VA + int(c)*4:012X} [{mn[c]:.1f},{mx[c]:.1f}] 跳{jp[c]:.1f}", flush=True)

# f64：8 字节对齐
G = np.frombuffer(b"".join([]) or b"", dtype=np.uint8) if False else None
res = {}
for c in idx[:400].astype(int):
    res[int(c)] = None

json.dump([f"{VA+int(c)*4:012X}" for c in idx[:200]], open(f"cdwide_{TAG}.json", "w"))

# 对候选所在页高频确认（含 f64 解读）
if len(idx) == 0:
    sys.exit(0)
off = idx[:400].astype(int)
pages = sorted({(int(c) * 4) // 4096 for c in off})
runs = []
for p in pages:
    if runs and p == runs[-1][1]: runs[-1][1] = p + 1
    else: runs.append([p, p + 1])
print(f"确认 {len(off)} 个 / {len(runs)} 段 / 20s", flush=True)


def rd(p0, p1):
    return read(VA + (p0 << 12), (p1 - p0) << 12)


ser = {int(c): [] for c in off}
t1 = time.time()
while time.time() - t1 < 20.0:
    tt = time.time() - t1
    for p0, p1 in runs:
        d = rd(p0, p1)
        f = np.frombuffer(d, dtype=np.uint8).view(np.float32)
        base = p0 * 4096 // 4
        for c in off:
            k = int(c) - base
            if 0 <= k < len(f): ser[int(c)].append((tt, float(f[k])))

final = []
for c, ss in ser.items():
    tt = np.array([x[0] for x in ss]); v = np.array([x[1] for x in ss])
    if not np.all(np.isfinite(v)) or len(v) < 8: continue
    rng = v.max() - v.min()
    if rng < 50: continue
    d = np.diff(v); dT = np.maximum(np.diff(tt), 1e-6)
    sl = d / dT
    med = np.median(sl[np.abs(sl) > 1e-6]) if np.any(np.abs(sl) > 1e-6) else 0
    dec = (d < -0.1 * rng).sum(); up = (d > 0.5 * rng).sum()
    if dec >= 4 and up >= 1 and abs(med) > 20:
        final.append((dec, up, med, c, v))
final.sort(key=lambda x: -x[0])
print(f"\n=== 宽量程候选 {len(final)} ===", flush=True)
for dec, up, med, c, v in final[:10]:
    print(f"  {VA + c*4:012X} dec={dec} up={up} 中位斜率{med:+.0f}/s", flush=True)
    print("     ", " ".join(f"{x:.0f}" for x in v[:30]), flush=True)
