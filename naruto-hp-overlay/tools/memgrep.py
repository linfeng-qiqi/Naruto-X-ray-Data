# -*- coding: utf-8 -*-
"""在进程 rw 匿名堆中搜索字符串/字节模式，返回命中地址"""
import subprocess, sys, time, struct
from concurrent.futures import ThreadPoolExecutor

ADB = r'D:/leidian/LDPlayer14/adb.exe'
PID = subprocess.run([ADB, 'shell', 'pidof com.tencent.KiHan'],
                     capture_output=True, text=True).stdout.split()[0]


def regions(perms='rw'):
    out = []
    for line in open('maps_new.txt'):
        p = line.split()
        if len(p) < 2:
            continue
        a, b = [int(x, 16) for x in p[0].split('-')]
        perm = p[1]
        name = p[5] if len(p) >= 6 else '[anon]'
        if any(c not in perm for c in perms):
            continue
        if name.startswith('/'):
            continue          # 跳过文件映射
        out.append((a, b, name))
    return out


def rd(a, n):
    r = subprocess.run([ADB, 'exec-out',
                        f'toybox dd if=/proc/{PID}/mem bs=4096 skip={a//4096} count={(n+0xFFF)//4096} 2>/dev/null'],
                       capture_output=True, timeout=180)
    return r.stdout[:n]


if __name__ == '__main__':
    pat = sys.argv[1].encode() if len(sys.argv) > 1 else b'Assembly-CSharp'
    regs = regions()
    tot = sum(b - a for a, b, _ in regs)
    print(f'rw 匿名段 {len(regs)} 共 {tot/1048576:.0f} MB  模式 {pat!r}', flush=True)
    CH = 0x800000
    jobs = []
    for a, b, n in regs:
        o = a
        while o < b:
            jobs.append((o, min(CH, b - o)))
            o += CH
    hits = []
    cnt = [0]

    def work(j):
        a, n = j
        d = rd(a, n)
        cnt[0] += 1
        if cnt[0] % 100 == 0:
            print(f'  {cnt[0]}/{len(jobs)}', flush=True)
        i = d.find(pat)
        while i >= 0:
            hits.append(a + i)
            i = d.find(pat, i + 1)

    t0 = time.time()
    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(work, jobs))
    print(f'耗时 {time.time()-t0:.0f}s 命中 {len(hits)}', flush=True)
    for h in hits[:40]:
        print(f'  {h:012X}', flush=True)
