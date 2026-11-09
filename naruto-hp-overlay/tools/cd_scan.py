# -*- coding: utf-8 -*-
"""三模态倒计时探测器（浮点倒计时 / 0-1进度条 / 整数秒阶梯）
用法：python cd_scan.py <中心VA> <±MB> [粗采秒]
"""
import subprocess, sys, time, json
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
CEN = int(sys.argv[1], 16)
RND = int(sys.argv[2]) if len(sys.argv) > 2 else 8
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 28.0
VA = (CEN - (RND << 20)) & ~0xFFF
SIZE = (RND * 2) << 20
print(f"pid={PID} 区间 {VA:012X} +{SIZE>>20}MB 粗采{SECS}s", flush=True)


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
dnn = np.zeros(N, np.float32)
bad = np.zeros(N, bool)
intg = np.zeros(N, np.float32)
cnt = 0
prev = None
t0 = time.time()
while time.time() - t0 < SECS:
    f = np.frombuffer(read(VA, SIZE), dtype=np.uint8).view(np.float32)
    np.minimum(mn, f, out=mn); np.maximum(mx, f, out=mx)
    bad |= ~np.isfinite(f)
    np.minimum(intg + (np.abs(f - np.round(f)) > 0.01), 1.0, out=intg)   # 0=全整数
    if prev is not None:
        np.maximum(jp, f - prev, out=jp)
        np.minimum(dnn, f - prev, out=dnn)
    prev = f
    cnt += 1
dt = (time.time() - t0) / cnt
print(f"{cnt} 帧 dt={dt:.2f}s", flush=True)

span = mx - mn
m = (~bad) & np.isfinite(span) & (
      ((mn <= 1.6) & (mx >= 7.5) & (mx <= 11.2) & (jp >= 4.0)) |          # A 10->0
      ((mn <= 0.06) & (mx >= 0.80) & (mx <= 1.05) & (jp >= 0.55)) |       # B 0->1
      ((intg == 0) & (mn >= -0.5) & (mx >= 5.0) & (mx <= 12.0) & (jp >= 4.0)) |  # C 整数阶梯
      ((mn <= 1.6) & (mx >= 7.5) & (mx <= 11.2) & (dnn <= -4.0))          # D 0->10 递增型
)
idx = np.nonzero(m)[0]
print(f"粗筛幸存 {len(idx)}", flush=True)
print("  " + " ".join(f"{VA + int(c)*4:012X}" for c in idx[:25]), flush=True)
if len(idx) == 0:
    json.dump([], open("cdscan_res.json", "w")); sys.exit(0)

off = idx[:300].astype(int)
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
        f = np.frombuffer(read((p0 << 12), (p1 - p0) << 12), dtype=np.uint8).view(np.float32)
        for c in off:
            k = int(c) - (p0 << 12) // 4
            if 0 <= k < len(f): ser[int(c)].append((tt, float(f[k])))

def score(vv):
    """返回 (是否通过, 说明)；量程小到 0.45 也算（fillAmount 0~1），方向不限"""
    rng = vv.max() - vv.min()
    if rng < 0.45: return None
    d = np.diff(vv)
    dec = (d < -0.02 * rng).sum(); inc = (d > 0.02 * rng).sum()
    resets = ((d > 0.5 * rng) | (d < -0.5 * rng)).sum()
    if resets >= 1 and ((dec >= 8 and inc <= 4) or (inc >= 8 and dec <= 4)):
        return f"降{dec} 升{inc} 跳回{resets} 量程{rng:.3f}"
    return None

res = []
for c, ss in ser.items():
    tt = np.array([a for a, _ in ss]); vv = np.array([b for _, b in ss])
    if not np.all(np.isfinite(vv)) or len(vv) < 20: continue
    s = score(vv)
    if s: res.append((len(vv), c, vv, s))
res.sort(key=lambda r: -r[0])
print(f"\n=== 候选 {len(res)} ===", flush=True)
for _, c, vv, s in res[:12]:
    print(f"  {VA + c*4:012X}  {s}", flush=True)
    print("     ", " ".join(f"{v:.2f}" for v in vv[:40]), flush=True)
json.dump([{"va": f"{VA + c*4:012X}", "info": s} for _, c, _, s in res[:30]],
          open("cdscan_res.json", "w"), indent=1, ensure_ascii=False)
