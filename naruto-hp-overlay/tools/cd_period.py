# -*- coding: utf-8 -*-
"""周期对齐探测：CD 周期 10s，则间隔 ~5s 两次采样，同一字段值必差 ≈5.0
（10->0 倒计时）或 ≈0.5（0->1 fillAmount）或整数差 5。
全堆一次覆盖，判据极窄，误报极少。
"""
import subprocess, time, json, sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
GAP = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
CH = 16 << 20

regs = []
r = subprocess.run([ADB, "shell", f"cat /proc/{PID}/maps"], capture_output=True, text=True)
for line in r.stdout.splitlines():
    p = line.split()
    if len(p) < 2 or not p[1].startswith("rw"): continue
    if "/" in " ".join(p[5:]): continue
    a, b = [int(x, 16) for x in p[0].split("-")]
    if b - a >= 1 << 20: regs.append((a, b))
total = sum(b - a for a, b in regs)
print(f"pid={PID} 区域 {len(regs)} 个 共 {total/1048576:.0f}MB", flush=True)

tasks = []
for a, b in regs:
    va = a
    while va < b:
        n = min(CH, b - va) & ~0xFFF
        if n: tasks.append((va, n))
        va += n
        if n == 0: break
print(f"分块 {len(tasks)}", flush=True)


def rd(t):
    va, n = t
    s = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null"],
        capture_output=True, timeout=180)
    d = s.stdout
    return d if len(d) == n else None


hits = []
WAVE = 14          # 每波 14 块 ≈ 224MB
t0 = time.time()
for w in range(0, len(tasks), WAVE):
    grp = tasks[w:w + WAVE]
    with ThreadPoolExecutor(max_workers=7) as ex:
        A = list(ex.map(rd, grp))
    time.sleep(GAP)
    with ThreadPoolExecutor(max_workers=7) as ex:
        B = list(ex.map(rd, grp))
    for (va, n), a, b in zip(grp, A, B):
        if a is None or b is None: continue
        fa = np.frombuffer(a, dtype=np.uint8).view(np.float32)
        fb = np.frombuffer(b, dtype=np.uint8).view(np.float32)
        d = fb - fa
        ok = np.isfinite(fa) & np.isfinite(fb)
        m = ok & (np.abs(np.abs(d) - GAP) < 0.45) & (np.minimum(fa, fb) > -0.25) & (np.maximum(fa, fb) < 10.7)
        m |= ok & (np.abs(np.abs(d) - 0.5) < 0.045) & (np.minimum(fa, fb) > -0.03) & (np.maximum(fa, fb) < 1.03)
        for c in np.nonzero(m)[0]:
            hits.append((int(va + c * 4), float(fa[c]), float(fb[c])))
    print(f"  波{w//WAVE+1}/{(len(tasks)+WAVE-1)//WAVE} 累计命中 {len(hits)}  {time.time()-t0:.0f}s", flush=True)

print(f"\n=== 周期对齐命中 {len(hits)} ===", flush=True)
for va, a, b in hits[:60]:
    print(f"  {va:012X}  {a:8.3f} -> {b:8.3f}   Δ={b-a:+.3f}", flush=True)
json.dump([{"va": f"{v:012X}", "a": a, "b": b} for v, a, b in hits],
          open(f"cd_period_{GAP:.1f}.json", "w"), indent=1)
