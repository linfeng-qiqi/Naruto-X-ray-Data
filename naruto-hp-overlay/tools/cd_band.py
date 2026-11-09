# -*- coding: utf-8 -*-
"""窄带扫描：已知全局时间 T(t)，CD 结束时间戳 E 必落在 [T-3, T+12]。
全堆一次扫过，只把落在窄带里的 float 捞出来 —— 命中数应该是个位数。
用法：python cd_band.py
"""
import subprocess, time, json
from concurrent.futures import ThreadPoolExecutor
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
TVAS = [0x72E5C0FC64F8, 0x72E5C11506C8, 0x72E5C11506D8]
LO, HI = -3.0, 12.0
CH = 16 << 20


def read(va, n):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null"],
        capture_output=True, timeout=180)
    d = r.stdout
    return d if len(d) == n else None


# 先建立 T 的线性模型
print("标定 T…", flush=True)
pages = {}
for va in TVAS:
    pages[va] = va & ~0xFFF
snap1 = {}
for va in TVAS:
    d = read(pages[va], 0x1000)
    snap1[va] = (time.time(), np.frombuffer(d, dtype=np.uint8).view(np.float32)[(va - pages[va]) // 4])
time.sleep(1.0)
T0 = {}
for va in TVAS:
    d = read(pages[va], 0x1000)
    now = time.time()
    v1 = float(np.frombuffer(d, dtype=np.uint8).view(np.float32)[(va - pages[va]) // 4])
    t1, v0 = snap1[va]
    el = now - t1
    T0[va] = (float(v0), v1)
    print(f"  T@{va:012X}: {v0:.4f} -> {v1:.4f}  (Δ{v1-v0:.4f} / 理论 {el:.2f})", flush=True)

regs = []
r = subprocess.run([ADB, "shell", f"cat /proc/{PID}/maps"], capture_output=True, text=True)
for line in r.stdout.splitlines():
    p = line.split()
    if len(p) < 2 or not p[1].startswith("rw"): continue
    if "/" in " ".join(p[5:]): continue
    x, y = [int(v, 16) for v in p[0].split("-")]
    if y - x >= 1 << 20: regs.append((x, y))
tasks = []
for x, y in regs:
    va = x
    while va < y:
        n = min(CH, y - va) & ~0xFFF
        if n: tasks.append((va, n))
        va += n
        if n == 0: break
print(f"区域 {len(regs)} 个 分块 {len(tasks)}", flush=True)

STATE = {"t0": None}
hits = []


def job(t):
    va, n = t
    d = read(va, n)
    if d is None: return
    now = time.time()
    f = np.frombuffer(d, dtype=np.uint8).view(np.float32)
    for tva, (v0, v1) in T0.items():
        T = v0 + (now - STATE["t0"])
        m = (f > T + LO) & (f < T + HI)
        for c in np.nonzero(m)[0]:
            hits.append((int(va + c * 4), float(f[c]), float(f[c] - T), tva))


t_start = time.time()
STATE["t0"] = t_start
with ThreadPoolExecutor(max_workers=7) as ex:
    list(ex.map(job, tasks))
print(f"扫完 {time.time()-t_start:.0f}s", flush=True)

hits.sort(key=lambda h: abs(h[2]))
print(f"\n=== 窄带命中 {len(hits)} ===", flush=True)
seen = set()
for va, v, rel, tva in hits:
    if va in seen: continue
    seen.add(va)
    print(f"  {va:012X}  V={v:10.3f}  V-T={rel:+7.3f}  基准{tva:012X}", flush=True)
json.dump([{"va": f"{v:012X}", "V": val, "rel": rel, "base": f"{b:012X}"}
           for v, val, rel, b in hits], open("cd_band.json", "w"), indent=1)
