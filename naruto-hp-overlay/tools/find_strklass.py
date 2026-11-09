# -*- coding: utf-8 -*-
"""在 rw 匿名堆里找 C# string 实例 -> 反推出 System.String 的 Il2CppClass 指针"""
import subprocess, struct, collections
import numpy as np
from concurrent.futures import ThreadPoolExecutor

ADB = r'D:/leidian/LDPlayer14/adb.exe'
PID = subprocess.run([ADB, 'shell', 'pidof com.tencent.KiHan'],
                     capture_output=True, text=True).stdout.split()[0]


def rw_regions():
    out = []
    for line in open('maps_new.txt'):
        p = line.split()
        if len(p) < 2:
            continue
        a, b = [int(x, 16) for x in p[0].split('-')]
        perm = p[1]
        name = p[5] if len(p) >= 6 else '[anon]'
        if 'r' in perm and 'w' in perm and not name.startswith('/'):
            out.append((a, b))
    return out


def rd(a, n):
    r = subprocess.run([ADB, 'exec-out',
                        f'toybox dd if=/proc/{PID}/mem bs=4096 skip={a//4096} count={(n+0xFFF)//4096} 2>/dev/null'],
                       capture_output=True, timeout=180)
    return r.stdout[:n]


klass_count = collections.Counter()
CH = 0x800000


def work(reg):
    a, b = reg
    o = a
    while o < b:
        n = min(CH, b - o)
        d = rd(o, n)
        m = len(d) // 8 * 8
        if m >= 64:
            q = np.frombuffer(d[:m], dtype='<u8')
            u = np.frombuffer(d[:m], dtype='<u4')
            nq = len(q)
            # 对象起点 k (字节, 8 对齐) -> q 下标 i=k/8
            # monitor(q[i+1])==0, len(u[2i+4]) in [2,40], 首字符 u16 可打印
            i = np.arange(0, nq - 8)
            mon = q[i + 1]
            ln = u[(i + 2) * 2]        # 字节偏移 k+16 => u4 下标 (k+16)/4 = 2i+4
            ch0 = np.frombuffer(d[:m], dtype='<u2')
            c0 = ch0[(i + 2) * 4 + 2]
            ok = (mon == 0) & (ln >= 2) & (ln <= 40) & (c0 >= 0x20) & (c0 < 0x7f)
            for k in i[ok]:
                klass_count[int(q[k])] += 1
        o += n


if __name__ == '__main__':
    regs = rw_regions()
    print(f'{len(regs)} 段 共 {sum(b-a for a,b in regs)/1048576:.0f} MB', flush=True)
    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(work, regs))
    print('候选 klass 值 top20:')
    for k, v in klass_count.most_common(20):
        print(f'  {k:012X}  {v}')


def q(va):
    d = rd(va & ~0xFFF, 0x1000)
    o = va & 0xFFF
    return struct.unpack('<Q', d[o:o+8])[0] if len(d) >= o + 8 else 0


def cstr(va, n=64):
    d = rd(va & ~0xFFF, 0x1000)
    o = va & 0xFFF
    s = d[o:o+n]
    e = s.find(b'\x00')
    return s[:e].decode('utf-8', 'replace') if e > 0 else None
