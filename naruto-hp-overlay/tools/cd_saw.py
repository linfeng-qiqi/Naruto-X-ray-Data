# -*- coding: utf-8 -*-
"""锯齿(倒计时)探测器
在给定的大块连续内存里，找"以 1/秒 递减、归零后跳回 ~10"的信号。
技能 CD=10 秒循环释放 -> 该信号必然存在。
阶段1：低采样率(约1Hz)扫全区，用 min/max/跳变 粗筛
阶段2：对幸存 offset 高频(约5Hz)采样，验证斜率 -1 与周期 ~10s
用法：python cd_saw.py [起始VA] [MB] [秒]
"""
import subprocess, sys, time, json
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PKG = "com.tencent.KiHan"
PID = subprocess.run([ADB, "shell", "pidof", PKG], capture_output=True, text=True).stdout.strip()
VA = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x72E5C0000000
MB = int(sys.argv[2]) if len(sys.argv) > 2 else 64
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 24.0
SIZE = MB << 20
print(f"pid={PID} 区域 {VA:012X} +{MB}MB 采样 {SECS}s", flush=True)


def read(va, n):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null"],
        capture_output=True, timeout=120)
    d = r.stdout
    if len(d) != n:
        d = d + b"\x00" * (n - len(d))
    return d


# ---------------- 阶段 1 ----------------
t0 = time.time()
prev_f = prev_i = None
mn = np.full(SIZE // 4, np.float32(1e30), dtype=np.float32)
mx = np.full(SIZE // 4, np.float32(-1e30), dtype=np.float32)
jump = np.zeros(SIZE // 4, dtype=np.float32)
bad = np.zeros(SIZE // 4, dtype=bool)
nf = 0
ts = []
while time.time() - t0 < SECS:
    ts.append(time.time())
    buf = np.frombuffer(read(VA, SIZE), dtype=np.uint8)
    f = buf.view(np.float32)
    np.minimum(mn, f, out=mn)
    np.maximum(mx, f, out=mx)
    bad |= ~np.isfinite(f)
    if prev_f is not None:
        d = f - prev_f
        np.maximum(jump, d, out=jump)
    prev_f = f
    nf += 1
    print(f"  帧{nf} t={ts[-1]-t0:5.1f}s", flush=True)

dt = (ts[-1] - ts[0]) / max(1, nf - 1)
print(f"阶段1 完成 {nf} 帧, dt={dt:.2f}s, 分析…", flush=True)

# 粗筛：全区有限、范围覆盖 [<=1.5 .. >=7.5]、跳回 >=4、有实质变化
m = (~bad) & (mn >= -0.5) & (mn <= 1.5) & (mx >= 7.5) & (mx <= 11.0) & (jump >= 4.0)
idx = np.nonzero(m)[0]
print(f"阶段1 幸存 {len(idx)} 个 offset", flush=True)
for c in idx[:40]:
    print(f"  {VA + c*4:012X}  min={mn[c]:.3f} max={mx[c]:.3f} jump={jump[c]:.2f}", flush=True)

json.dump([f"{VA + int(c)*4:012X}" for c in idx[:400]], open("cd_saw_surv.json", "w"))
if len(idx) == 0:
    print("无幸存者"); sys.exit(0)

# ---------------- 阶段 2 ----------------
off = [int(c) for c in idx[:400]]
# 只读幸存者所在页
pages = sorted(set((c * 4) // 4096 for c in off))
runs = []
for p in pages:
    if runs and p == runs[-1][1]: runs[-1][1] = p + 1
    else: runs.append([p, p + 1])
print(f"\n阶段2 高频监控 {len(off)} 个 offset / {len(runs)} 段, 18s", flush=True)

ser = {c: [] for c in off}
t1 = time.time()
while time.time() - t1 < 18.0:
    tt = time.time() - t1
    for p0, p1 in runs:
        d = read(VA + (p0 << 12), (p1 - p0) << 12)
        f = np.frombuffer(d, dtype=np.uint8).view(np.float32)
        base = (p0 << 12) // 4
        for c in off:
            ln = c - base
            if 0 <= ln < len(f):
                ser[c].append((tt, float(f[ln])))
    time.sleep(0.03)
print(f"  采样完成 {len(ser[off[0]])} 帧", flush=True)

final = []
for c, ss in ser.items():
    tt = np.array([a for a, _ in ss]); vv = np.array([b for _, b in ss])
    if not np.all(np.isfinite(vv)): continue
    best = 0.0; best_seg = ()
    i = 0
    while i < len(vv) - 1:
        j = i
        while j + 1 < len(vv):
            dtt = tt[j+1] - tt[j]
            if dtt <= 0: break
            sl = (vv[j+1] - vv[j]) / dtt
            if -1.4 < sl < -0.6 and vv[j] <= 11.5:
                j += 1
            else:
                break
        dur = tt[j] - tt[i]
        if dur > best:
            best = dur; best_seg = (i, j)
        i = max(j, i + 1)
    if best < 5.5: continue
    i, j = best_seg
    after = vv[j:min(len(vv), j + 12)]
    idx_up = [k for k in range(j, min(len(vv) - 1, j + 12)) if vv[k+1] - vv[k] > 3.5]
    if not idx_up: continue
    final.append((best, c, vv, i, j))
final.sort(key=lambda r: -r[0])
print(f"\n=== 最终 CD 候选 {len(final)} 个 ===", flush=True)
for best, c, vv, i, j in final[:12]:
    print(f"  {VA + c*4:012X} 连续递减 {best:.1f}s", flush=True)
    print("     ", " ".join(f"{v:.2f}" for v in vv[max(0,i-2):j+8]), flush=True)
json.dump([{"va": f"{VA + c*4:012X}", "run_s": round(b, 1)} for b, c, _, _, _ in final],
          open("cd_saw_final.json", "w"), indent=1)
