# -*- coding: utf-8 -*-
"""不限量纲的"周期性倒计时/进度"探测器。
能同时抓：10->0 秒、0->1 fillAmount、0->10 蓄力、0->1000 毫秒 等。
判据：值长期在一条窄带内、单调走完大半量程、然后跳回，周期 6~14 秒。
用法：python cd_any.py <起始VA> <MB> [粗采秒]
"""
import subprocess, sys, time, json
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
VA = int(sys.argv[1], 16)
MB = int(sys.argv[2])
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 24.0
SIZE = MB << 20
TAG = f"{VA:012X}"
print(f"pid={PID} {TAG} +{MB}MB {SECS}s", flush=True)


def read(va, n):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null"],
        capture_output=True, timeout=120)
    d = r.stdout
    return d + b"\x00" * (n - len(d)) if len(d) != n else d


t0 = time.time()
mn = None; mx = None; jump = None; prev = None; bad = None
nf = 0; ts = []
while time.time() - t0 < SECS:
    ts.append(time.time())
    f = np.frombuffer(read(VA, SIZE), dtype=np.uint8).view(np.float32)
    if mn is None:
        mn = f.copy(); mx = f.copy(); jump = np.zeros_like(f); bad = ~np.isfinite(f)
    else:
        np.minimum(mn, f, out=mn); np.maximum(mx, f, out=mx)
        bad |= ~np.isfinite(f)
        np.maximum(jump, f - prev, out=jump)
    prev = f
    nf += 1

dt = (ts[-1] - ts[0]) / max(1, nf - 1)
span = mx - mn
print(f"{nf} 帧 dt={dt:.2f}s", flush=True)
m = (~bad) & np.isfinite(span) & (span >= 1.5) & (span <= 12.0) & (jump >= 0.55 * span)
idx = np.nonzero(m)[0]
print(f"粗筛幸存 {len(idx)}", flush=True)
if len(idx) == 0:
    json.dump([], open(f"cdany_{TAG}.json", "w")); sys.exit(0)

off = idx[:250].astype(int)
pages = sorted(set((int(c) * 4) // 4096 for c in off))
runs = []
for p in pages:
    if runs and p == runs[-1][1]: runs[-1][1] = p + 1
    else: runs.append([p, p + 1])
print(f"高频确认 {len(off)} 个 / {len(runs)} 段 / 20s", flush=True)

ser = {int(c): [] for c in off}
t1 = time.time()
while time.time() - t1 < 20.0:
    tt = time.time() - t1
    for p0, p1 in runs:
        f = np.frombuffer(read(VA + (p0 << 12), (p1 - p0) << 12), dtype=np.uint8).view(np.float32)
        base = (VA + (p0 << 12) - VA) // 4
        for c in off:
            k = int(c) - base
            if 0 <= k < len(f): ser[int(c)].append((tt, float(f[k])))

res = []
for c, ss in ser.items():
    tt = np.array([a for a, _ in ss]); vv = np.array([b for _, b in ss])
    if not np.all(np.isfinite(vv)) or len(vv) < 20: continue
    d = np.diff(vv); dT = np.maximum(np.diff(tt), 1e-6)
    up_idx = np.nonzero(d > 0.5 * (vv.max() - vv.min()))[0]
    if len(up_idx) < 1: continue
    # 相邻两次跳回间隔 = 周期
    per = np.diff(tt[up_idx]) if len(up_idx) >= 2 else np.array([])
    okper = per[(per > 6) & (per < 15)]
    # 单调性：跳回之间应基本单调
    mono = 0; tot = 0
    bounds = list(up_idx) + [len(vv) - 1]
    for k in range(len(bounds) - 1):
        a, b = bounds[k] + 1, bounds[k + 1]
        if b - a < 5: continue
        seg = vv[a:b + 1]
        dd = np.diff(seg)
        tot += 1
        if abs(dd.sum()) > 0.6 * (vv.max() - vv.min()) and (np.sign(dd).sum() and abs(np.sign(dd).mean()) > 0.85):
            mono += 1
    score = len(okper) * 2 + mono
    if len(okper) >= 1 or (mono >= 1 and len(up_idx) >= 1):
        res.append((score, len(okper), mono, c, vv, okper))

res.sort(key=lambda r: -r[0])
print(f"\n=== {TAG} 候选 {len(res)} ===", flush=True)
for sc, nper, mono, c, vv, per in res[:10]:
    pv = ",".join(f"{p:.1f}" for p in per[:4])
    print(f"  {VA + c*4:012X} score={sc} 周期命中{nper}[{pv}] 单调段{mono} range[{vv.min():.2f},{vv.max():.2f}]", flush=True)
    print("     ", " ".join(f"{v:.2f}" for v in vv[:36]), flush=True)
json.dump([{"va": f"{VA + c*4:012X}", "score": s, "periods": [round(float(p), 2) for p in per[:4]]}
           for s, _, _, c, _, per in res[:20]], open(f"cdany_{TAG}.json", "w"), indent=1)
