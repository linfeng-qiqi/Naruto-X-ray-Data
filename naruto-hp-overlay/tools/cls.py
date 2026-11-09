# -*- coding: utf-8 -*-
"""il2cpp 运行时内省：从任意托管对象拿到 klass，再从 klass 读 name / namespace / fields"""
import subprocess, struct, sys, time

ADB = r'D:/leidian/LDPlayer14/adb.exe'


def get_pid():
    r = subprocess.run([ADB, 'shell', 'pidof com.tencent.KiHan'], capture_output=True, text=True).stdout.split()
    if not r:
        raise SystemExit('游戏没在跑')
    return r[0]


PID = get_pid()


def read(va, n):
    n = max(0x1000, (n + 0xFFF) & ~0xFFF)
    r = subprocess.run([ADB, 'exec-out',
                        f'toybox dd if=/proc/{PID}/mem bs=4096 skip={va//4096} count={n//4096} 2>/dev/null'],
                       capture_output=True, timeout=120)
    return r.stdout


def rd(va, n):
    d = read(va & ~0xFFF, (va & 0xFFF) + n)
    o = va & 0xFFF
    return d[o:o+n] if len(d) >= o + n else b''


def q(va):
    d = rd(va, 8)
    return struct.unpack('<Q', d)[0] if len(d) == 8 else 0


def cstr(va, maxlen=96):
    if va < 0x1000:
        return None
    d = rd(va, maxlen)
    if not d:
        return None
    e = d.find(b'\x00')
    if e < 0:
        return None
    s = d[:e]
    if not s or len(s) > 80:
        return None
    try:
        t = s.decode('utf-8')
    except Exception:
        return None
    if not all(32 <= ord(c) < 127 or c in '\u4e00-\u9fff' for c in t):
        return None
    return t


def klass_name(klass):
    n = cstr(q(klass + 0x10))
    ns = cstr(q(klass + 0x18))
    return n, ns


def probe(va):
    """dump 一个对象的 klass 并打印结构"""
    print(f'== 对象 {va:012X} ==')
    k = q(va)
    print(f'  klass = {k:012X}')
    n, ns = klass_name(k)
    print(f'  name = {n!r}  namespace = {ns!r}')
    print('  --- klass 前 0x140 字节 ---')
    d = rd(k, 0x140)
    for off in range(0, len(d), 8):
        v = struct.unpack('<Q', d[off:off+8])[0]
        tag = ''
        s = cstr(v) if 0x10000 < v < 0x800000000000 else None
        if s:
            tag = f'  -> "{s}"'
        print(f'   +{off:03X} {v:016X}{tag}')


if __name__ == '__main__':
    if len(sys.argv) > 1:
        probe(int(sys.argv[1], 16))
