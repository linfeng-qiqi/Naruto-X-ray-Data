# -*- coding: utf-8 -*-
"""在进程内存中搜索解密后的 global-metadata.dat 头 (0xFAB11BAF)"""
import subprocess, struct, sys, time
from concurrent.futures import ThreadPoolExecutor

ADB = r'D:/leidian/LDPlayer14/adb.exe'
PID = subprocess.run([ADB, 'shell', 'pidof com.tencent.KiHan'],
                     capture_output=True, text=True).stdout.strip().split()[0]
print('PID', PID, flush=True)

maps = subprocess.run([ADB, 'shell', f'cat /proc/{PID}/maps'],
                      capture_output=True, text=True).stdout
regions = []
for line in maps.splitlines():
    p = line.split()
    if len(p) < 2:
        continue
    a, b = [int(x, 16) for x in p[0].split('-')]
    if 'r' not in p[1]:
        continue
    name = p[5] if len(p) >= 6 else ''
    regions.append((a, b, name))
tot = sum(b - a for a, b, _ in regions)
print(f'可读段 {len(regions)} 共 {tot/1048576:.0f} MB', flush=True)

CHUNK = 0x800000
jobs = []
for a, b, name in regions:
    o = a
    while o < b:
        jobs.append((o, min(CHUNK, b - o), name))
        o += CHUNK
print(f'分块 {len(jobs)}', flush=True)

MAGIC = b'\xaf\x1b\xb1\xfa'
hits = []
lock = 0
done = [0]


def read(a, n):
    r = subprocess.run([ADB, 'exec-out', f'toybox dd if=/proc/{PID}/mem bs=4096 skip={a//4096} count={(n+0xFFF)//4096} 2>/dev/null'],
                       capture_output=True, timeout=120)
    return r.stdout[:n]


def work(job):
    a, n, name = job
    d = read(a, n)
    done[0] += 1
    if done[0] % 40 == 0:
        print(f'  {done[0]}/{len(jobs)}  {done[0]*CHUNK/1048576:.0f}MB', flush=True)
    if not d:
        return
    i = d.find(MAGIC)
    while i >= 0:
        va = a + i
        # 校验 version 合理 (20~32) 且 后面字段像偏移
        try:
            ver = struct.unpack('<i', d[i+4:i+8])[0]
        except Exception:
            ver = -1
        rec = (va, ver, name)
        if 0 < ver < 100:
            hits.append(rec)
            print(f'  命中 {va:012X} version={ver} {name}', flush=True)
        i = d.find(MAGIC, i + 1)


t0 = time.time()
with ThreadPoolExecutor(max_workers=8) as ex:
    list(ex.map(work, jobs))
print(f'耗时 {time.time()-t0:.1f}s  命中 {len(hits)}', flush=True)
for h in hits:
    print(f'{h[0]:012X} v={h[1]} {h[2]}', flush=True)
