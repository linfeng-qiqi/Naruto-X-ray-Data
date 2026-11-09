# il2cpp 运行时内省（不解析 metadata）

传统做法是 `Il2CppDumper`：拉 `libil2cpp.so` + `global-metadata.dat`，离线解析出全部类、字段、方法，再套地址。

这个游戏**走不通**：`global-metadata.dat` 的头部被加密了。

```
磁盘上的文件头：94 43 72 12  18 4B 9E EC  66 67 66 66 ...
正常 il2cpp：   AF 1B B1 FA  (magic)      1F 00 00 00  (version 31)
```

magic 不对，版本号也是乱的。但**字符串区是明文** —— 在文件偏移 `0xB7F73` 能找到 `Assembly-CSharp`。也就是说只有头部（section 偏移表）被加密，正文没动。

试过在运行时内存里搜 `AF 1B B1 FA` 找解密后的副本，全量 56 GB 扫下来 0 命中 —— 说明解密是分块/按需做的，或头部被改成别的值。

## 换一条路：不做离线解析，直接读运行时的类结构

绕开 metadata 文件，改从**活对象**反推。原理只有三条：

1. 每个 C# 对象的**前 8 字节**就是它的 `Il2CppClass*`；
2. `Il2CppClass` 里 `name` / `namespaze` 是两个 `char*`，指向明文字符串；
3. `Il2CppClass.fields` 指向 `FieldInfo` 数组，步长 `0x20`，里面直接有**字段名和偏移**。

不需要 metadata，也不需要知道游戏版本。

### 第一步：拿到一个已知类的指针

需要一个「锚点」把整套机制撬开。用 `System.String`：

C# string 对象的布局是固定的：

```
+0x00   Il2CppClass*  klass
+0x08   void*         monitor   (通常是 0)
+0x10   int32         length
+0x14   uint16[]      chars     (UTF-16)
```

于是全堆扫这个形状：`monitor == 0`、`length ∈ [2, 40]`、首字符是可打印 ASCII，然后统计每个命中的 `klass` 值出现了多少次 —— 字符串实例有 50 万个，`System.String` 的类指针会以压倒性优势排第一：

```
72E5D027C1B0   528679      <-- System.String 的 Il2CppClass*
000000000000    13984
0000FFFFFFFF       84
...
```

`tools/find_strklass.py` 就是这个脚本（rw 匿名堆 1.9 GB，8 线程约 40 秒）。

### 第二步：读类信息

```python
name      = cstr(q(klass + 0x10))     # "String"
namespace = cstr(q(klass + 0x18))     # "System"
fields    = q(klass + 0x80)           # FieldInfo[]
```

`tools/ic.py` 的输出：

```
klass 72E5D027C1B0  System.String
  fields @klass+0x80  (7 项)
      StackallocIntBufferSizeLimit   off=0x0000
      PROBABILISTICMAP_BLOCK_INDEX_MASK  off=0x0000
      _stringLength                  off=0x0010
      _firstChar                     off=0x0014
      Empty                          off=0x0000
```

`_stringLength = 0x10`、`_firstChar = 0x14` —— 和 string 对象真实布局完全一致，证明**字段表是可信的**。

### 第三步（如果要做到底）

拿到 `System.String` 的 klass 之后，顺着 `Il2CppClass.image → Il2CppImage.types` 就能枚举出整个 assembly 的类表，再按名字筛 `Skill` / `CD` / `Cool` 之类，把字段偏移读出来，最后用 klass 值在全堆里反查实例。

这条链没走完（见 [04-逆向日志.md](04-逆向日志.md)），但前面三步的代码是通的，缺的只是最后一段枚举。

## 注意事项

- **类表每次重启会重排**：实测同一个 klass 地址在换号/重启后读不到（页面被回收），工具已改成「先试默认偏移，失败就扫描定位」。
- `FieldInfo` 校验时用 `type` 指针落在 `libil2cpp` 的 `0x4000_0000_0000` 区间这一条，能过滤掉绝大多数误判。
- `FieldInfo` 里 `offset = 0` 的通常是 `static` 字段，不在对象实例里，别当成实例字段去读。
