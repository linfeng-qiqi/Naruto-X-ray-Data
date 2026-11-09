# -*- coding: utf-8 -*-
# 漏斗筛选：
#  A) 粗采 6 轮（并行双读），统计每个候选的最大观测值。
#     技能 CD 放出去会从 10 开始掉 -> 一定能观测到 >=7；
#     特效计时器多为 0.3~1.5，永远到不了 7 → 一次筛掉绝大多数。
#  B) 幸存者（少）高频监控 30 秒，确认锯齿（-1/s 递减 >=8 次 + 跳回 >=1 次）。
# 进战斗循环放 2 技能，约 2 分钟。
import subprocess, time, os, sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
print("pid =", PID, flush=True)

cands = {}
for line in open("cd_cands.txt"):
    p = line.split(); cands[int(p[0], 16)] = (float(p[1]), float(p[2]))
print(f"候选 {len(cands)}", flush=True)

pages = sorted(set(va >> 12 for va in cands))
runs = []
for p in pages:
    if runs and p == runs[-1][1]: runs[-1][1] = p + 1
    else: runs.append([p, p + 1])
run_list = [((p0 << 12), (p1 - p0) << 12) for p0, p1 in runs]
print(f"{len(runs)} 段", flush=True)

def read_chunk(va, n):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} conv=noerror,sync"],
        capture_output=True, timeout=90)
    d = r.stdout[:n]
    return d if len(d) == n else None

def sweep():
    out = {}
    with ThreadPoolExecutor(max_workers=10) as ex:
        futs = {ex.submit(read_chunk, va, n): (va, n) for va, n in run_list}
        for fu in futs:
            va, n = futs[fu]
            d = fu.result()
            if d is not None:
                out[(va >> 12) << 12] = np.frombuffer(d, dtype=np.float32)
    return out

# ---------- A) 粗采 ----------
samples = {}
ROUNDS = 6
for rd in range(ROUNDS):
    t1 = time.time()
    A = sweep()
    time.sleep(1.05)
    t2 = time.time()
    B = sweep()
    for va in cands:
        base = (va >> 12) << 12
        off = (va - base) // 4
        for tt, arr in ((t1, A.get(base)), (t2, B.get(base))):
            if arr is None or off >= len(arr): continue
            samples.setdefault(va, []).append((tt, float(arr[off])))
    print(f"  轮{rd+1}/{ROUNDS}  dt={time.time()-t1:.1f}s  样本点{len(samples)}", flush=True)

stats = []
for va, ss in samples.items():
    vals = [v for _, v in ss]
    if not vals: continue
    mx = max(vals)
    inr = sum(1 for v in vals if 0.0 <= v <= 10.6)
    if mx >= 7.0 and inr >= len(vals) * 0.7:
        stats.append((va, mx, len(vals), inr))
stats.sort(key=lambda r: -r[1])
print(f"\nA 阶段幸存 {len(stats)} 个（曾观测到 >=7 秒）", flush=True)
for va, mx, n, ir in stats[:30]:
    print(f"  {va:012X} max={mx:.2f} 样本{n} 在域{ir}")

surv = [va for va, _, _, _ in stats]
with open("cd_surv.txt", "w") as fh:
    for va in surv: fh.write(f"{va:X}\n")
if not surv:
    print("无幸存者，退出"); sys.exit(0)

# ---------- B) 高频监控幸存者 ----------
spages = sorted(set(va >> 12 for va in surv))
sruns = []
for p in spages:
    if sruns and p == sruns[-1][1]: sruns[-1][1] = p + 1
    else: sruns.append([p, p + 1])
srun_list = [((p0 << 12), (p1 - p0) << 12) for p0, p1 in sruns]
print(f"\nB 阶段：{len(surv)} 个地址 / {len(srun_list)} 段，高频监控 30 秒", flush=True)

def sweep_small():
    out = {}
    with ThreadPoolExecutor(max_workers=8) as ex:
        futs = {ex.submit(read_chunk, va, n): (va, n) for va, n in srun_list}
        for fu in futs:
            va, n = futs[fu]
            d = fu.result()
            if d is not None:
                out[(va >> 12) << 12] = np.frombuffer(d, dtype=np.float32)
    return out

t0 = time.time()
while time.time() - t0 < 30:
    A = sweep_small()
    ta = time.time()
    for va in surv:
        base = (va >> 12) << 12
        off = (va - base) // 4
        arr = A.get(base)
        if arr is not None and off < len(arr):
            samples.setdefault(va, []).append((ta, float(arr[off])))
    time.sleep(0.1)

print("B 阶段采样完成，分析…", flush=True)
final = []
for va in surv:
    ss = sorted(samples[va])
    dec = up = inr = 0
    for (t1, v1), (t2, v2) in zip(ss, ss[1:]):
        dt = t2 - t1
        if dt <= 0: continue
        if 0.05 < v1 <= 10.5:
            inr += 1
            if abs((v2 - v1) + dt) < 0.15: dec += 1
        if v2 - v1 > 2.0 and v2 <= 10.6: up += 1
    if dec >= 8 and up >= 1:
        final.append((va, dec, up, inr))
final.sort(key=lambda r: (-r[1], -r[2]))
print(f"\n=== 最终 CD 候选 {len(final)} 个 ===")
for va, d, u, ir in final[:20]:
    ss = sorted(samples[va])
    wave = " ".join(f"{v:.2f}" for _, v in ss[-25:])
    print(f"  {va:012X}  dec={d} up={u}  尾部波形: {wave}")
with open("cd_final4.txt", "w") as fh:
    for va, d, u, ir in final: fh.write(f"{va:X} {d} {u} {ir}\n")
