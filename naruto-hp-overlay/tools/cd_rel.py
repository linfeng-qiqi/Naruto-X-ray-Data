# -*- coding: utf-8 -*-
"""相对时间验证：对窄带命中的地址，逐帧算 R = V - T。
真 CD：R 从 10 递减到 0（斜率 -1），然后跳回 ~10。其它时间戳则恒定不动。
用法：python cd_rel.py [秒]
"""
import subprocess, sys, time, json
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 22.0
TBASE = 0x72E5C0FC64F8

h = json.load(open("cd_band.json"))
addrs = sorted({int(x["va"], 16) for x in h if -1.5 <= x["rel"] <= 11.5})
print(f"候选 {len(addrs)}（已按 R∈[-1.5,11.5] 收敛）", flush=True)


def read(va, n):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null"],
        capture_output=True, timeout=90)
    d = r.stdout
    return d + b"\x00" * (n - len(d)) if len(d) != n else d


want = set(addrs) | {TBASE}
pages = sorted({a >> 12 for a in want})
GAP = 48                      # 页间隔 ≤48 页（192KB）就并成一段读
runs = []
for p in pages:
    if runs and p - runs[-1][1] <= GAP: runs[-1][1] = p + 1
    else: runs.append([p, p + 1])
tot = sum((b - a) * 4096 for a, b in runs)
print(f"{len(runs)} 段 共 {tot/1048576:.1f}MB", flush=True)

ser = {a: [] for a in addrs}
from concurrent.futures import ThreadPoolExecutor
POOL = ThreadPoolExecutor(max_workers=10)
t0 = time.time()
nf = 0
while time.time() - t0 < SECS:
    tt = time.time() - t0
    days = [POOL.submit(read, p0 << 12, (p1 - p0) << 12) for p0, p1 in runs]
    vals = {}
    for (p0, p1), fu in zip(runs, days):
        f = np.frombuffer(fu.result(), dtype=np.uint8).view(np.float32)
        base = (p0 << 12)
        for a in want:
            if a in vals or a < base or a >= base + len(f) * 4: continue
            vals[a] = float(f[(a - base) // 4])
    T = vals.get(TBASE)
    if T is None: continue
    for a in addrs:
        if a in vals:
            ser[a].append((tt, vals[a] - T))
    nf += 1
print(f"{nf} 帧 {nf/SECS:.1f}Hz", flush=True)

hits = []
loose = []
for a, ss in ser.items():
    if len(ss) < 8: continue
    tt = np.array([x[0] for x in ss]); r = np.array([x[1] for x in ss])
    if not np.all(np.isfinite(r)): continue
    if r.min() < -2.0 or r.max() > 12.0: continue
    if r.max() - r.min() < 2.5: continue
    d = np.diff(r); dT = np.maximum(np.diff(tt), 1e-6)
    sl = d / dT
    dec = ((sl < -0.25) & (sl > -1.9)).sum()
    up = (d > 3.5).sum()
    loose.append((dec, up, a, r))
    if dec >= 5 and up >= 1 and r.min() < 3.0 and r.max() > 6.0:
        hits.append((dec, up, a, r))
hits.sort(key=lambda x: (-x[0], -x[1]))
loose.sort(key=lambda x: (-x[1], -x[0]))
print(f"\n=== R 递减候选 {len(hits)} ===", flush=True)
for dec, up, a, r in hits[:10]:
    print(f"  {a:012X}  dec={dec} up={up}  R范围[{r.min():.2f},{r.max():.2f}]", flush=True)
    print("     ", " ".join(f"{v:.2f}" for v in r[:36]), flush=True)
if not hits:
    print("  (无) —— 放宽列出有跳回的:", flush=True)
    for dec, up, a, r in [x for x in loose if x[1] >= 1][:8]:
        print(f"  {a:012X} dec={dec} up={up} R[{r.min():.2f},{r.max():.2f}]", flush=True)
        print("     ", " ".join(f"{v:.2f}" for v in r[:36]), flush=True)
json.dump([{"va": f"{a:012X}", "dec": d, "up": u, "rmin": float(r.min()), "rmax": float(r.max())}
           for d, u, a, r in hits[:20]], open("cd_rel_res.json", "w"), indent=1)
