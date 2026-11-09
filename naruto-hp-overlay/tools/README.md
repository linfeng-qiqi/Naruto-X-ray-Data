# tools

> **⚠️ 仅供学习研究参考，请勿违法使用！** 以下脚本仅用于理解进程内存分析技术，请勿用于联网游戏或任何违法用途。

逆向分析用的 Python 脚本。全部依赖 **adb**（路径写在脚本顶部的 `ADB` 常量里），部分需要 `numpy`。

```bash
python -m pip install numpy
```

所有脚本都假定你已经在 `build/` 目录下有一个 `maps_new.txt`（进程内存映射快照），生成方式：

```bash
adb shell "cat /proc/$(adb shell pidof com.tencent.KiHan)/maps" > maps_new.txt
```

## 通用内存工具

| 脚本 | 作用 |
|---|---|
| `memgrep.py <字符串>` | 在 rw 匿名堆里搜字符串或字节模式，返回所有命中地址 |
| `find_strklass.py` | 反查 `System.String` 的 `Il2CppClass*`（全堆 1.9 GB，约 40 秒） |
| `ic.py <klass>` | 类内省：打印类名、命名空间、全部字段名与偏移 |
| `ic.py --strclass` | 等价于 `find_strklass.py` |
| `cls.py <对象地址>` | 从任意 C# 对象反查它属于哪个类 |
| `md_find.py` | 在内存里搜解密后的 `global-metadata.dat` 头（本项目 0 命中） |

## 技能冷却定位（全部失败，留作案例）

| 脚本 | 思路 |
|---|---|
| `cd_period.py <间隔秒>` | **周期对齐法**：CD 恰 10 秒 → 隔 5 秒两次采样必差 5.0 |
| `cd_scan.py <base> <MB> <秒>` | 多模态扫描（0→10 / 0→1 / 整数阶梯），按页二次确认 |
| `cd_page.py <地址> <页数> <秒>` | 按页 30 Hz 高频采样，打印每个字段的波形 |
| `cd_saw.py <base> <MB> <秒>` | 不限量纲的锯齿检测 |
| `cd_band.py` | 先标定全局时间 T，再扫 `[T, T+10]` 窄带 |
| `cd_rel.py <秒>` | 逐帧算 `值 - T` 的相对时间，验证是否从 10 递减到 0 |
| `cd_time.py <base> <MB> <秒>` | 找全局时间源 / 每秒 +1 的阶梯 |
| `cd_wide.py <base> <MB> <秒>` | 毫秒量程（0~10000）+ f64 双精度 |
| `cd_tree.py <记录地址> <秒>` | 沿指针两跳展开对象树，列出所有变化字段 |
| `cd_obj.py` / `cd_rec.py` | 单对象 / 整条记录逐字段跟踪 |
| `cd_any.py <base> <MB> <秒>` | 无预设模式的通用倒计时探测 |
| `cd_funnel.py` | 漏斗筛选：粗采留「曾 ≥7 秒」的候选，再高频确认锯齿 |

## 数据

`data/address.cache.sample` 是一份定位结果的样例（含静态链结构），`data/cd_cands.sample.txt` 是 5545 个「每秒 -1」候选地址的样例 —— 都是死数据，只用来对照格式。

## 注意

- 脚本里的 `PID` 通常是启动时 `pidof` 拿的，**游戏重启后要重跑**；
- 这些脚本会大量并发读内存，实测**把模拟器读到崩过**（游戏进程直接消失），跑之前存档；
- 地址全部是某一局的值，换个号就失效，**只有偏移和结构是稳定的**。
