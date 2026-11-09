# -*- coding: utf-8 -*-
"""顺着属性记录里的指针，逐个对象高频采样，找 10 秒倒计时锯齿。
用法：python cd_obj.py <记录基址hex> [秒]
"""
import subprocess, sys, time
import numpy as np

ADB = r"D:/leidian/LDPlayer14/adb.exe"
PID = subprocess.run([ADB, "shell", "pidof", "com.tencent.KiHan"],
                     capture_output=True, text=True).stdout.strip()
REC = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x72E53BB256C0
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0
OBJ = 0x400


def rd(va, pages):
    r = subprocess.run([ADB, "exec-out",
        f"toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={pages} 2>/dev/null"],
        capture_output=True, timeout=60)
    return r.stdout


def read_at(va, n):
    base = va & ~0xFFF
    pages = ((va - base + n) + 0xFFF) // 0x1000
    d = rd(base, pages)
    s = d[va - base: va - base + n]
    return s + b"\x00" * (n - len(s)) if len(s) != n else s


raw = read_at(REC, 0x240)
ptrs = []
import struct
for off in range(0, 0x240 - 7, 8):
    q = struct.unpack("<Q", raw[off:off + 8])[0]
    if 0x700000000000 <= q < 0x800000000000 and q not in ptrs:
        ptrs.append(q)
print(f"记录 {REC:012X} 里的指针 {len(ptrs)} 个", flush=True)
for p in ptrs: print(f"   -> {p:012X}", flush=True)

# 按页归组，只读指针所在的对象区
targets = [(p, p) for p in ptrs]
ser = {}
t0 = time.time()
nf = 0
while time.time() - t0 < SECS:
    tt = time.time() - t0
    for p in ptrs:
        d = read_at(p, OBJ)
        ser.setdefault(p, []).append(np.frombuffer(d, dtype=np.uint8).view(np.float32))
    nf += 1
print(f"{nf} 帧 {nf/SECS:.1f}Hz", flush=True)

print("\n=== 各对象里的 10 秒锯齿 ===")
found = 0
for p, frames in ser.items():
    A = np.stack(frames)
    mn = A.min(axis=0); mx = A.max(axis=0)
    span = mx - mn
    cand = np.nonzero(np.isfinite(mn) & (mn >= -0.5) & (mn <= 1.6) & (mx >= 7.5) & (mx <= 11.2) & (span >= 4))[0]
    for c in cand:
        s = A[:, c].astype(np.float64)
        d = np.diff(s)
        dec = ((d < -0.15) & (d > -3.0)).sum()
        up = (d > 3.5).sum()
        if dec >= 6 and up >= 1:
            print(f"  {p + int(c)*4:012X} (对象{p:012X}+{int(c)*4:03X}) dec={dec} up={up} "
                  f"[{s.min():.2f},{s.max():.2f}]", flush=True)
            print("     ", " ".join(f"{v:.2f}" for v in s[:34]), flush=True)
            found += 1
    # 顺带报 0~1 的 fillAmount 锯齿
    cand = np.nonzero(np.isfinite(mn) & (mn <= 0.06) & (mx >= 0.80) & (mx <= 1.05) & (span >= 0.6))[0]
    for c in cand:
        s = A[:, c].astype(np.float64)
        d = np.diff(s)
        dec = ((d < -0.02) & (d > -0.6)).sum()
        up = (d > 0.4).sum()
        if dec >= 8 and up >= 1:
            print(f"  [fill] {p + int(c)*4:012X} dec={dec} up={up} [{s.min():.2f},{s.max():.2f}]", flush=True)
            print("     ", " ".join(f"{v:.3f}" for v in s[:34]), flush=True)
            found += 1
if not found:
    print("  （无）")
