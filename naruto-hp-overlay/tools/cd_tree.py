# -*- coding: utf-8 -*-
"""从属性记录出发，两跳内所有对象，高频采样，列出全部"变化过"的浮点字段。
不预设模式，人工/程序二次判断。
用法：python cd_tree.py <记录基址hex> [秒]
"""
import subprocess, sys, time, struct
from concurrent.futures import ThreadPoolExecutor
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
REC = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x72E53BB256C0
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
OBJ = 0x200
POOL = ThreadPoolExecutor(max_workers=12)


def read_at(va, n):
    base = va & ~0xFFF
    pages = ((va - base + n) + 0xFFF) // 0x1000
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={base//4096} count={pages} 2>/dev/null"],
        capture_output=True, timeout=60)
    d = r.stdout
    s = d[va - base: va - base + n]
    return s + b"\x00" * (n - len(s)) if len(s) != n else s


def ptrs_of(va):
    raw = read_at(va, 0x200)
    out = []
    for off in range(0, 0x200 - 7, 8):
        q = struct.unpack("<Q", raw[off:off + 8])[0]
        if 0x700000000000 <= q < 0x800000000000:
            out.append(q)
    return out


lvl1 = ptrs_of(REC)
lvl2 = []
for p in lvl1:
    lvl2 += ptrs_of(p)
targets = []
for p in [REC] + lvl1 + lvl2:
    if p not in targets: targets.append(p)
targets = targets[:60]
print(f"一跳 {len(lvl1)}，两跳合计 {len(targets)} 个对象，每个 {OBJ} 字节", flush=True)

frames = {p: [] for p in targets}
t0 = time.time()
nf = 0
while time.time() - t0 < SECS:
    futs = {p: POOL.submit(read_at, p, OBJ) for p in targets}
    for p, fu in futs.items():
        frames[p].append(np.frombuffer(fu.result(), dtype=np.uint8).view(np.float32))
    nf += 1
print(f"{nf} 帧 {nf/SECS:.1f}Hz", flush=True)

print("\n=== 变化过的浮点字段 ===")
tot = 0
for p in targets:
    A = np.stack(frames[p])
    mn = A.min(axis=0); mx = A.max(axis=0)
    ch = np.nonzero(np.isfinite(mx - mn) & ((mx - mn) > 0.004))[0]
    if len(ch) == 0: continue
    for c in ch:
        s = A[:, c].astype(np.float64)
        if not np.all(np.isfinite(s)): continue
        if abs(s).max() > 1e5: continue
        d = np.diff(s)
        dec = (d < -0.03).sum(); up = (d > 0.3).sum()
        tag = ""
        if dec >= 6 and up >= 1: tag = "  <<< 锯齿"
        print(f"  {p + int(c)*4:012X} (obj{p:012X}+{int(c)*4:03X}) "
              f"[{s.min():.3f},{s.max():.3f}] dec{dec} up{up}{tag}", flush=True)
        print("     ", " ".join(f"{v:.3f}" for v in s[:30]), flush=True)
        tot += 1
print(f"\n共 {tot} 个变化字段")
