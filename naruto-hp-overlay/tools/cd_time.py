# -*- coding: utf-8 -*-
"""时间源探测：找「全局时间」与「endTime 阶梯」

假设 A（endTime 模型，Unity 很常见）：
    内存里没有 0~10 的剩余时间，只有
      T = 全局时间，每秒 +1，单调不回头
      E = 结束时间戳 = 施放瞬间的 T + 10，其余时间恒定，每 10 秒 +10

检测：
    1) 斜率恒为 +1 的浮点（T）
    2) 阶梯信号：长时间恒定 + 每次跳变 ≈ +10（E）
用法：python cd_time.py <起始VA> <MB> [秒]
"""
import subprocess, sys, time, json
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
VA = int(sys.argv[1], 16)
MB = int(sys.argv[2])
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
SIZE = MB << 20
N = SIZE // 4
TAG = f"{VA:012X}"
print(f"pid={PID} {TAG} +{MB}MB {SECS}s", flush=True)


def read(va, n):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null"],
        capture_output=True, timeout=120)
    d = r.stdout
    return d + b"\x00" * (n - len(d)) if len(d) != n else d


# 流式记录：首帧、末帧、最小/最大增量一致性
first = None; last = None; dmin = None; dmax = None
up_cnt = None; dn_cnt = None
bad = np.zeros(N, bool)
ts = []
t0 = time.time()
while time.time() - t0 < SECS:
    ts.append(time.time())
    f = np.frombuffer(read(VA, SIZE), dtype=np.uint8).view(np.float32)
    f = np.nan_to_num(f)          # 独立可写副本，消掉 nan/inf
    if first is None:
        first = f.copy(); last = f.copy()
        dmin = np.full(N, np.float32(1e30), np.float32)
        dmax = np.full(N, np.float32(-1e30), np.float32)
        up_cnt = np.zeros(N, np.int16); dn_cnt = np.zeros(N, np.int16)
    else:
        d = f - last
        np.minimum(dmin, d, out=dmin); np.maximum(dmax, d, out=dmax)
        up_cnt += (d > 1e-4); dn_cnt += (d < -1e-4)
        last = f.copy()

dt = (ts[-1] - ts[0]) / max(1, len(ts) - 1)
print(f"{len(ts)} 帧 dt={dt:.2f}s", flush=True)

tot = last - first
el = ts[-1] - ts[0]
# ---- 1) 全局时间：整段单调增，总增量 ≈ 经过秒数，且每一帧都在增 ----
m1 = ((dn_cnt == 0) & (up_cnt >= len(ts) - 2)
      & (np.abs(tot - el) < 0.35 * el))
i1 = np.nonzero(m1)[0]
print(f"\n[1] 全局时间候选 {len(i1)}")
for c in i1[:12]:
    print(f"   {VA + int(c)*4:012X}  {first[c]:.3f} -> {last[c]:.3f}  总增{tot[c]:.3f} 经过{el:.2f}s")

# ---- 2) endTime 阶梯：大部分帧不动，跳变幅度 ≈ +10（容许 ±1.5）----
moved = (np.abs(tot) > 5.0) & (np.abs(tot) < 45.0)
still = (up_cnt + dn_cnt) <= max(3, int(len(ts) * 0.45))
m2 = moved & still & (dn_cnt == 0)
i2 = np.nonzero(m2)[0]
print(f"\n[2] 阶梯(endTime)候选 {len(i2)}")
for c in i2[:15]:
    print(f"   {VA + int(c)*4:012X}  {first[c]:.3f} -> {last[c]:.3f}  总增{tot[c]:+.3f} 动{int(up_cnt[c])}次")

json.dump({"time": [f"{VA+int(c)*4:012X}" for c in i1[:40]],
           "step": [f"{VA+int(c)*4:012X}" for c in i2[:60]]},
          open(f"cdtime_{TAG}.json", "w"), indent=1)
