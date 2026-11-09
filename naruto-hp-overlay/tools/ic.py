# -*- coding: utf-8 -*-
"""
Il2CppClass 运行时内省（免 metadata 解析）

思路：
  1. 任意一个 C# 对象的前 8 字节就是它的 Il2CppClass*；
  2. Il2CppClass 里 name / namespaze 是两个 char*，指向明文字符串；
  3. fields 是一个 FieldInfo 数组，步长 0x20，首字段是字段名字符串指针，
     +0x18 是字段在对象内的偏移，+0x08 是 Il2CppType*（落在 libil2cpp 的 0x4000_xxxxxxx 区）。

本版本实测：name@+0x10  namespaze@+0x18  fields@+0x80（Unity 2022 / il2cpp v29 系）。
不同版本这几个偏移会变，所以下面都是「先按默认试，失败就扫描定位」。

用法：
  python ic.py <klass 或 对象地址 hex>
  python ic.py --strclass        # 全堆反查 System.String 的 klass（528k 命中那个就是）
"""
import subprocess, struct, sys, collections
from concurrent.futures import ThreadPoolExecutor

ADB = r'D:/leidian/LDPlayer14/adb.exe'
PID = subprocess.run([ADB, 'shell', 'pidof com.tencent.KiHan'],
                     capture_output=True, text=True).stdout.split()[0]

NAME_OFF = 0x10
NS_OFF = 0x18
FIELDS_OFF = 0x80
SO_LO, SO_HI = 0x400000000000, 0x400060000000      # libil2cpp 代码/数据映射区间


def rd(va, n):
    """按页读取，自动处理跨页；读不到就返回能读到的部分"""
    o = va & 0xFFF
    pages = (o + n + 0xFFF) // 0x1000
    r = subprocess.run([ADB, 'exec-out',
                        f'toybox dd if=/proc/{PID}/mem bs=4096 skip={(va & ~0xFFF)//4096} count={pages} 2>/dev/null'],
                       capture_output=True, timeout=120)
    d = r.stdout
    return d[o:o + n] if len(d) >= o + n else b''


def q(va):
    d = rd(va, 8)
    return struct.unpack('<Q', d)[0] if len(d) == 8 else 0


def cstr(va, n=64):
    if not (0x10000 < va < 0x800000000000):
        return None
    d = rd(va, n)
    e = d.find(b'\x00')
    if e <= 0:
        return None
    try:
        t = d[:e].decode('utf-8')
    except Exception:
        return None
    return t if all(32 <= ord(c) < 127 for c in t) and len(t) <= 60 else None


def scan_string_ptrs(base, size=0x400):
    """列出结构体里所有指向可读字符串的指针 -> {偏移: 字符串}"""
    d = rd(base, size)
    out = {}
    for off in range(0, len(d) - 7, 8):
        v = struct.unpack('<Q', d[off:off + 8])[0]
        t = cstr(v)
        if t:
            out[off] = t
    return out


def try_fields(ptr, limit=48):
    """校验 ptr 是否是 FieldInfo 数组（步长 0x20）"""
    d = rd(ptr, 0x20 * limit)
    out = []
    for k in range(min(limit, len(d) // 0x20)):
        ent = d[k*0x20:(k+1)*0x20]
        namep, typep = struct.unpack('<QQ', ent[:16])
        off, tok = struct.unpack('<iI', ent[0x18:0x20])
        nm = cstr(namep, 40)
        if not nm:
            break
        if not (SO_LO <= typep < SO_HI):
            break
        if off < -1 or off > 0x8000:
            break
        out.append((nm, off, tok))
    return out or None


def find_fields(klass):
    """先试默认偏移，不行就在整个结构里扫"""
    cand = []
    f = try_fields(q(klass + FIELDS_OFF))
    if f:
        cand.append((FIELDS_OFF, f))
    if not cand:
        d = rd(klass, 0x600)
        for off in range(0, len(d) - 8, 8):
            p = struct.unpack('<Q', d[off:off + 8])[0]
            if not (0x10000 < p < 0x800000000000):
                continue
            f = try_fields(p)
            if f and len(f) >= 2:
                cand.append((off, f))
    return cand


def dump(klass):
    nm, ns = cstr(q(klass + NAME_OFF)), cstr(q(klass + NS_OFF))
    if not nm:
        # 布局不同：扫一遍，name 和 namespace 通常是相邻的一对
        sp = scan_string_ptrs(klass, 0x300)
        for off, s in sp.items():
            if off + 8 in sp:
                nm, ns = s, sp[off + 8]
                print(f'! 自适应定位：name@{off:#x} namespace@{off+8:#x}')
                break
    print(f'klass {klass:012X}  {(ns + ".") if ns else ""}{nm}')
    for off, f in find_fields(klass):
        print(f'  fields @klass+{off:#04x}  ({len(f)} 项)')
        for name, o, tok in f:
            print(f'      {name:<34} off=0x{o:04X}')


def find_str_class():
    """反查 System.String 的 Il2CppClass：string 对象 = klass, monitor(0), len@0x10, chars@0x14"""
    regs = []
    for line in open('maps_new.txt'):
        p = line.split()
        if len(p) < 2:
            continue
        a, b = [int(x, 16) for x in p[0].split('-')]
        if 'r' in p[1] and 'w' in p[1] and (len(p) < 6 or not p[5].startswith('/')):
            regs.append((a, b))
    cnt = collections.Counter()
    CH = 0x800000

    def work(reg):
        a, b = reg
        o = a
        while o < b:
            n = min(CH, b - o)
            r = subprocess.run([ADB, 'exec-out',
                                f'toybox dd if=/proc/{PID}/mem bs=4096 skip={o//4096} count={n//4096} 2>/dev/null'],
                               capture_output=True, timeout=180)
            d = r.stdout[:n]
            m = len(d) // 8 * 8
            if m >= 64:
                import numpy as np
                qq = np.frombuffer(d[:m], dtype='<u8')
                u4 = np.frombuffer(d[:m], dtype='<u4')
                u2 = np.frombuffer(d[:m], dtype='<u2')
                i = np.arange(0, len(qq) - 8)
                ln = u4[(i + 2) * 2]
                c0 = u2[(i + 2) * 4 + 2]
                ok = (qq[i + 1] == 0) & (ln >= 2) & (ln <= 40) & (c0 >= 0x20) & (c0 < 0x7f)
                for k in i[ok]:
                    cnt[int(qq[k])] += 1
            o += n

    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(work, regs))
    for k, v in cnt.most_common(5):
        print(f'  {k:012X}  {v}')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--strclass':
        find_str_class()
    elif len(sys.argv) > 1:
        dump(int(sys.argv[1], 16))
    else:
        print(__doc__)
