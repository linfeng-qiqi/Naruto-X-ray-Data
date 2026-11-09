// ============================================================================
//  火影忍者 · 内存血条叠加层 (只读，不写内存)
//
//  ！！ 免责声明 ！！
//  本项目仅供学习、研究与技术参考，请勿违法使用。
//  严禁用于联网对战、破坏游戏公平性、侵犯他人计算机信息系统或任何违法用途。
//  请在单机 / 离线 / 自建环境中研究，使用后果自负，作者不承担任何责任。
//
//  双方血量，各有【两份值】（结构一致、owner 相同；间距双方不同：
//    实测我方 0x46C0 / 敌方 0x900，由 ini 的 pair 分别配置，绝不能写死 0x900）：
//    值A = 扣血结算后才更新     值B = 扣血瞬间就变、与血条同步
//  F1 / F2 全局切换用哪一路（我方和敌方同时切换），主血条区域画两条：
//    上行 我方（红橙，从左往右填充）  下行 敌方（蓝紫，从右往左镜像填充）
//
//  定位方式（2026-11-09 最终版）：exe 内置自动定位，打开就显示，零手工操作、零 Python 依赖。
//    1) 启动时先读 address.cache 里上次成功的血块 VA，形状校验通过就立刻显示（<1 秒）。
//    2) 缓存失效（换号 / 重启模拟器 / 换角色）时，程序自己读 /proc/<pid>/maps，
//       按 HP 块结构特征（float hp + 0 区 + int 1 + 0x38 处 24 字节 TAIL）分块扫描，
//       同 owner 配成「同一实体的两份血量」，自动判定我方/敌方，写回缓存。
//    3) 稳态只读已定位的几页，不重走链、不串台、不蹦迪；血块消失就自动重定位。
//    真正常量只有「模块内静态根偏移」和「HP 块结构特征」——它们编译在 exe 里；
//    运行时 VA 必然随进程漂移，由程序自己每次找到，用户完全不需要管。
//
//  面板统计「A先变 / B先变」：扣血瞬间哪一路先动计数就涨（双方各自统计）。
//
//  overlay.ini：[chain] 我方 (mod+root+n+offN+va+pair)；[enemy] 敌方（各自 pair）；
//               [mode] default=a|b 设启动机制、show_enemy=1|0 设是否画敌方条。
//  换号 / 换角色 / 重启游戏 / 重启模拟器：一律自动接管，不会提示运行任何脚本。
//  没进战斗时显示「等待战斗」，一旦血量块出现立即自动定位并显示。
//
//  用法：
//    火影忍者.exe                      正常叠加（打开即自动定位）
//    火影忍者.exe --dump               打印当前定位结果、四路 VA/值/校验
//    火影忍者.exe --locate             只跑一次自动定位并打印结果
//    火影忍者.exe --preview x.png      渲染面板存盘
//    火影忍者.exe --preview x.png fake [b]   假数据自检渲染（可指定 F2）
//
//  热键（无需窗口焦点，全局轮询）：
//    F1     全局用 [值A]        F2     全局用 [值B]
//    F3     显示 / 隐藏敌方条   F4     复位(值A + 显示敌方)
//    F5     强制重新解引用两条链 + 重新定位 pair（切场景/重启后用）
//    F6+↑↓←→  微调叠加层位置（1px）  F7 复位  F8 显隐  F12 存图  F9 退出
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <clocale>
#include <cmath>
#include <algorithm>
#include <thread>
#include <sstream>
#include <fstream>
#include <cwchar>
#include <process.h>
#include "zlib/zlib.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ------------------------------------------------------------------ 数据结构

struct Candidate {
    std::wstring name;      // 显示名
    uintptr_t    addr;      // guest 虚拟地址（仅作展示用）
    bool         isRatio;   // 恒为 true（火影血量是 0~1 比例）
    float        fixedMax;  // 未使用
    float        peak;      // 未使用
    float        value;     // 当前读数
    bool         ok;        // 本次读取是否成功
};

struct HpGroup {
    float                   hp;   // 该组血量值（0~1）
    std::vector<uint64_t>  vas;  // 该组命中的 guest 虚拟地址（同值副本）
};

struct ViewCfg {
    double x = 0.09378;  // 叠加层左上角，相对模拟器客户区宽高的比例
    double y = 0.16057;
    double w = 0.16870;  // 主血条宽度比例（与游戏真血条等宽同左沿，便于比对长度）
    double h = 0.01249;  // 主血条高度比例
};

// ------------------------------------------------------------------ 全局状态

static std::vector<Candidate> g_cands;
static std::vector<uint64_t>  g_candVa;   // 与各候选对应的读取地址
static ViewCfg  g_view;
static int      g_mech       = 0;          // 机制：0=值A(结算)  1=值B(扣血同步)
static bool     g_showEnemy  = true;       // 是否绘制敌方血条
static bool     g_visible   = true;
static bool     g_nudgeMode = false;
static int      g_nudgeDX   = 0;
static int      g_nudgeDY   = 0;

static std::wstring g_iniPath;
static std::wstring g_exeDir;
static std::wstring g_winProcName = L"dnplayer.exe";  // 模拟器界面外壳（仅用于定位窗口）
static std::wstring g_adbPath     = L"D:\\leidian\\LDPlayer14\\adb.exe";
static std::wstring g_adbSer      = L"emulator-5554";

// ====================== 内置常量（直接编译进 exe）======================
// 【真静态根】libil2cpp.so + 0x452E4A8：CE 里是绿色地址，两次真实重启实证不变。
// 下面两条链只作 CE 参考 / 离线诊断，运行时定位不依赖它们：
//   腾讯加固壳每次启动都重建 il2cpp 类表，整串偏移跨进程会变（本次实证 root 就从
//   0x452E4A8 变成 0x4913E20 / 0x5601570）。真正跨进程不变的是「HP 块结构特征」，
//   exe 用它自己找当前进程的血块 VA；模块根只用于 --dump 里显示基址。
static const uint64_t DEF_MOD_ROOT = 0x452E4A8ULL;   // libil2cpp.so + 模块内偏移
static const uint32_t DEF_OFFS_SELF[] = {
    0x3230, 0xB8, 0x150, 0x118, 0x60, 0xA0, 0x30, 0x178
};
static const uint32_t DEF_OFFS_ENEMY[] = {
    0x1DEA0, 0x20, 0xB8, 0x120, 0xE0, 0x10, 0x80, 0x0, 0x10, 0x28, 0x178
};
static const int DEF_N_SELF  = (int)(sizeof(DEF_OFFS_SELF)  / sizeof(DEF_OFFS_SELF[0]));
static const int DEF_N_ENEMY = (int)(sizeof(DEF_OFFS_ENEMY) / sizeof(DEF_OFFS_ENEMY[0]));
static const uint32_t DEF_PAIR = 0x900;   // 同一实体两份血量块的间距

#define MAX_CHAIN_OFFS 16   // 静态字段链实测 11 级（类表 -> +0xB8 static_fields -> 单例 -> HP）
static uint32_t g_pairDelta = DEF_PAIR;  // 双方共用，仅用于显示

// root 写成「libil2cpp.so 基址 + 模块内偏移」时，启动时自动从 /proc/<pid>/maps
// 重定位模块基址。托管堆 VA 每次重启都会漂移，绝对堆地址不是静态地址。
static uint64_t g_modBase = 0;
static int      g_basePid = 0;

// 每一方（0=我方 / 1=敌方）自己的链根、N 级偏移、pair 缓存与两路读数。
struct SideState {
    uint64_t root      = 0;       // modRoot=true 时，这里存的是模块内偏移
    bool     modRoot   = false;
    uint64_t directVa  = 0;       // 进程内稳定锚：血块值A 的 VA（优先于链，战斗中不会被重建带偏）
    uint64_t directVaB = 0;       // 值B 的 VA。实测同一实体两路 owner 可能不同，必须直接记地址
    int      nOff      = 4;       // 链级数：off[0..nOff-2] 解引用，off[nOff-1] 是 HP 块内偏移
    uint32_t off[MAX_CHAIN_OFFS] = { 0 };
    uint32_t pairDelta = 0x900;
    uint64_t pairPage  = 0;      // 值A 所在 4KB 页
    uint32_t pairOffA  = 0;      // 值A 页内偏移
    uint32_t pairOffB  = 0;      // 值B 相对该页的偏移（可能 >= 0x1000）
    uint64_t pairOwner = 0;      // owner 指针（TAIL 校验用）
    bool     ready     = false;  // pair 缓存是否已定位
    bool     ok        = false;  // 本次读取是否成功
    float    a = 0.f, b = 0.f;  // 值A / 值B
    uint64_t vaA = 0, vaB = 0, owner = 0;
    uint32_t delta = 0;
    float    lastA = 0.f, lastB = 0.f;
    bool     init  = false;
    int      leadA = 0, leadB = 0;   // 扣血瞬间「谁先变」累计次数
};
static SideState g_sides[2];
static int       g_pairTick = 0;     // 快速刷新轮数（每 10 轮重走一次链）

// ---------------- 内置自动定位状态（全部由 exe 自己维护） ----------------
// 真正跨重启不变的是「HP 块结构特征」和「模块内静态根偏移」；运行时 VA 每次都会漂。
// 程序每次启动自己找 VA 并写进 exe 同目录的 address.cache，下次启动秒开。
enum LocState { LOC_INIT = 0, LOC_BUSY = 1, LOC_OK = 2, LOC_WAIT = 3 };
static volatile LONG g_locState    = LOC_INIT;  // 定位状态
static volatile LONG g_locProgress = 0;         // 定位进度 0..100
static volatile LONG g_needLocate  = 1;         // 置 1 = 扫描线程重新定位
static std::wstring  g_locNote;                 // 状态说明（g_cs 保护）
static wchar_t       g_locNoteSnap[128] = L""; // UI 线程用的快照
static int           g_locStateSnap    = LOC_INIT;
static int           g_locProgressSnap = 0;
static int           g_cachePid       = 0;
static uint64_t      g_cacheVa[2]     = { 0, 0 };
static uint64_t      g_cacheVaB[2]    = { 0, 0 };
static uint32_t      g_cachePair[2]   = { 0x900, 0x900 };
static DWORD         g_lastProbe      = 0;      // 上次 adb 设备探测时间
static bool          g_adbReady       = false;
static uint64_t      g_scanBudget     = 0;      // 本次定位已读字节数（仅诊断）
static bool          g_deepScan       = false;  // --deep：不提前停止，枚举全部血量块（诊断用）
static float         g_barL = 1.0f, g_barR = 1.0f;  // 定位时实测的左右血条比例

// 扫描/读取线程共享
static int                g_pid       = 0;
static bool               g_haveData  = false;
static bool               g_scanning  = false;
static bool               g_exit      = false;
static HANDLE             g_scanEvent = nullptr;
static CRITICAL_SECTION   g_cs;

// ------------------------------------------------------------------ 小工具

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring(L".") : s.substr(0, p);
}

static void EnsureIni() {
    g_exeDir = ExeDir();
    g_iniPath = g_exeDir + L"\\overlay.ini";
    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 模板与编译进 exe 的内置静态地址保持一致；删掉本文件也不会退回旧的绝对 VA。
        const wchar_t* def =
            L"; 火影忍者 血条叠加层配置（自动生成，与 exe 内置静态地址一致）\n"
            L"; 静态根 = libil2cpp.so + 0x452E4A8，模块基址启动时从 /proc/<pid>/maps 重定位；\n"
            L";   两次真实重启实证根 VA 恒为 40002BB2E4A8，CE 里是绿色静态地址。\n"
            L"; n = 链级数，off0..off(n-1) 十六进制，末级 0x178 是 HP 块内偏移。\n"
            L"; va / pair 由程序自动定位后写入同目录 address.cache，无需手工填写。\n"
            L"; 换号 / 重启游戏 / 重启模拟器 / 换角色：exe 自己重新定位，不依赖任何脚本。\n"
            L"; ini 只管显示参数与 adb 设置；找不到 adb 或设备时程序会自动探测。\n"
            L"\n"
            L"[adb]\n"
            L"path=D:\\leidian\\LDPlayer14\\adb.exe\n"
            L"serial=emulator-5554\n"
            L"\n"
            L"[proc]\n"
            L"window=dnplayer.exe\n"
            L"\n"
            L"[view]\n"
            L"x=0.09378\n"
            L"y=0.16057\n"
            L"w=0.16870\n"
            L"h=0.01249\n"
            L"\n"
            L"[chain]\n"
            L"mod=libil2cpp.so\n"
            L"root=452E4A8\n"
            L"n=8\n"
            L"off0=3230\n"
            L"off1=B8\n"
            L"off2=150\n"
            L"off3=118\n"
            L"off4=60\n"
            L"off5=A0\n"
            L"off6=30\n"
            L"off7=178\n"
            L"pair=900\n"
            L"\n"
            L"[enemy]\n"
            L"mod=libil2cpp.so\n"
            L"root=452E4A8\n"
            L"n=11\n"
            L"off0=1DEA0\n"
            L"off1=20\n"
            L"off2=B8\n"
            L"off3=120\n"
            L"off4=E0\n"
            L"off5=10\n"
            L"off6=80\n"
            L"off7=0\n"
            L"off8=10\n"
            L"off9=28\n"
            L"off10=178\n"
            L"\n"
            L"[mode]\n"
            L"default=a\n"
            L"show_enemy=1\n";
        HANDLE h = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const wchar_t bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(h, &bom, sizeof(bom), &written, nullptr);
            WriteFile(h, def, (DWORD)(wcslen(def) * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(h);
        }
    }
}

static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512] = {0};
    GetPrivateProfileStringW(sec, key, def, buf, 512, g_iniPath.c_str());
    return std::wstring(buf);
}

static double IniDouble(const wchar_t* sec, const wchar_t* key, double def) {
    std::wstring s = IniStr(sec, key, L"");
    if (s.empty()) return def;
    return _wtof(s.c_str());
}

static void InitConfig() {
    EnsureIni();
    std::wstring ap = IniStr(L"adb", L"path", L"");
    if (!ap.empty()) g_adbPath = ap;
    std::wstring se = IniStr(L"adb", L"serial", L"");
    if (!se.empty()) g_adbSer = se;
    g_winProcName = IniStr(L"proc", L"window", L"dnplayer.exe");
    g_view.x = IniDouble(L"view", L"x", g_view.x);
    g_view.y = IniDouble(L"view", L"y", g_view.y);
    g_view.w = IniDouble(L"view", L"w", g_view.w);
    g_view.h = IniDouble(L"view", L"h", g_view.h);

    // 链参数：root 可以是绝对 VA，也可以是「模块基址 + 模块内偏移」（sec 里写了 mod 时）。
    // 链级数由 n 决定（2~MAX_CHAIN_OFFS），off[n-1] 是 HP 块内偏移，其余为解引用偏移。
    static const wchar_t* CK[MAX_CHAIN_OFFS] = {
        L"off0",  L"off1",  L"off2",  L"off3",  L"off4",  L"off5",  L"off6",  L"off7",
        L"off8",  L"off9",  L"off10", L"off11", L"off12", L"off13", L"off14", L"off15"
    };
    static const wchar_t* SECS[2] = { L"chain", L"enemy" };
    for (int k = 0; k < 2; ++k) {
        SideState& S = g_sides[k];
        // 先装入编译进 exe 的静态根链：overlay.ini 缺失/损坏时也直接可用。
        S.root    = DEF_MOD_ROOT;
        S.modRoot = true;
        S.nOff    = (k == 0) ? DEF_N_SELF : DEF_N_ENEMY;
        memset(S.off, 0, sizeof(S.off));
        memcpy(S.off, (k == 0) ? DEF_OFFS_SELF : DEF_OFFS_ENEMY,
               sizeof(uint32_t) * S.nOff);
        S.pairDelta = DEF_PAIR;
        S.directVa  = 0;

        const wchar_t* sec = SECS[k];
        std::wstring rv = IniStr(sec, L"root", L"");
        if (!rv.empty()) {
            // ini 显式给了根：写了 mod 就按「模块基址 + 模块内偏移」，否则按旧格式绝对 VA
            try { S.root = std::stoull(rv, nullptr, 16); } catch (...) {}
            S.modRoot = !IniStr(sec, L"mod", L"").empty();
        } else if (k == 1) {
            S.root    = g_sides[0].root;
            S.modRoot = g_sides[0].modRoot;
        }
        std::wstring ns = IniStr(sec, L"n", L"");
        int n = ns.empty() ? (rv.empty() ? S.nOff : 4) : (int)_wtol(ns.c_str());
        S.nOff = (n >= 2 && n <= MAX_CHAIN_OFFS) ? n : S.nOff;
        for (int i = 0; i < S.nOff; ++i) {
            std::wstring s = IniStr(sec, CK[i], L"");
            if (!s.empty()) {
                try { S.off[i] = (uint32_t)std::stoul(s, nullptr, 16); } catch (...) {}
            } else if (k == 1 && i < 4 && !rv.empty()) {
                S.off[i] = g_sides[0].off[i];   // 兼容旧 ini：敌方缺省沿用我方
            }
        }
        std::wstring pp = IniStr(sec, L"pair", L"");
        if (!pp.empty()) {
            try { S.pairDelta = (uint32_t)std::stoul(pp, nullptr, 16); } catch (...) {}
        } else if (k == 1) {
            S.pairDelta = g_sides[0].pairDelta;
        }
        // 进程内稳定锚：ini 写了 va 就优先按 VA 直读，链只作重启后的重定位备用。
        std::wstring dv = IniStr(sec, L"va", L"");
        if (!dv.empty()) {
            try { S.directVa = std::stoull(dv, nullptr, 16); } catch (...) { S.directVa = 0; }
        }
    }
    g_pairDelta = g_sides[0].pairDelta;

    std::wstring md = IniStr(L"mode", L"default", L"");
    if (md == L"b" || md == L"B" || md == L"1") g_mech = 1;
    std::wstring she = IniStr(L"mode", L"show_enemy", L"");
    if (she == L"0" || she == L"false" || she == L"no") g_showEnemy = false;
}

// 用户手工改 overlay.ini（叠加层位置 / adb 路径）时热重载显示参数。
// 地址一律由 exe 自己写进 address.cache，不经过 ini，也不会重置已定位的血块。
static FILETIME g_iniStamp{};

static uint64_t FtVal(const FILETIME& f) {
    return ((uint64_t)f.dwHighDateTime << 32) | (uint64_t)f.dwLowDateTime;
}
static bool IniChanged() {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &fad)) return false;
    uint64_t v = FtVal(fad.ftLastWriteTime);
    if (!v) return false;
    uint64_t old = FtVal(g_iniStamp);
    g_iniStamp = fad.ftLastWriteTime;
    if (!old) return false;             // 首次只是记录基线
    return v != old;
}

// ------------------------------------------------------------------ adb 封装

static std::string g_lastAdbErr;      // 最近一次 adb 的 stderr（诊断用）

// 以管道捕获 adb 文本输出。binary=false 时去掉尾部 \0。
static bool RunAdb(const std::wstring& args, std::string& out, bool binary = false) {
    std::wstring cmd = L"\"" + g_adbPath + L"\" -s " + g_adbSer + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    HANDLE hOutR = nullptr, hOutW = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) return false;

    // stderr 单独一根管道收集：dd 的“records in/out”汇总混进二进制流会把 gzip 冲坏；
    // 但也不能直接丢 NUL —— adb 自身报错要能看见，否则 0 字节时无从排查。
    HANDLE hErrR = nullptr, hErrW = nullptr;
    if (!CreatePipe(&hErrR, &hErrW, &sa, 0)) { CloseHandle(hOutR); CloseHandle(hOutW); return false; }
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hOutW;
    si.hStdError  = hErrW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        g_lastAdbErr = "CreateProcess failed, err=" + std::to_string(GetLastError()) +
                       " cmd=" + std::string(cmd.begin(), cmd.end());
        CloseHandle(hOutR); CloseHandle(hOutW);
        CloseHandle(hErrR); CloseHandle(hErrW);
        return false;
    }
    CloseHandle(hOutW); CloseHandle(hErrW);

    std::vector<char> data;
    char tmp[8192]; DWORD got;
    while (ReadFile(hOutR, tmp, sizeof(tmp), &got, nullptr) && got > 0)
        data.insert(data.end(), tmp, tmp + got);
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hOutR);

    std::string err;
    while (ReadFile(hErrR, tmp, sizeof(tmp), &got, nullptr) && got > 0)
        err.append(tmp, got);
    CloseHandle(hErrR);
    if (!err.empty()) g_lastAdbErr = err;

    if (binary) {
        out.assign(data.begin(), data.end());
    } else {
        size_t n = data.size();
        while (n > 0 && data[n - 1] == 0) n--;
        out.assign(data.begin(), data.begin() + n);
    }
    return true;
}

// 以管道捕获 adb 的二进制原始输出（如 screencap -p）
static bool RunAdbBin(const std::wstring& args, std::vector<BYTE>& out) {
    std::wstring cmd = L"\"" + g_adbPath + L"\" -s " + g_adbSer + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    HANDLE hOutR = nullptr, hOutW = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) return false;

    // stderr 单独一根管道收集：dd 的“records in/out”汇总混进二进制流会把 gzip 冲坏；
    // 但也不能直接丢 NUL —— adb 自身报错要能看见，否则 0 字节时无从排查。
    HANDLE hErrR = nullptr, hErrW = nullptr;
    if (!CreatePipe(&hErrR, &hErrW, &sa, 0)) { CloseHandle(hOutR); CloseHandle(hOutW); return false; }
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hOutW;
    si.hStdError  = hErrW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hOutR); CloseHandle(hOutW);
        CloseHandle(hErrR); CloseHandle(hErrW);
        return false;
    }
    CloseHandle(hOutW); CloseHandle(hErrW);

    std::vector<char> data;
    char tmp[8192]; DWORD got;
    while (ReadFile(hOutR, tmp, sizeof(tmp), &got, nullptr) && got > 0)
        data.insert(data.end(), tmp, tmp + got);
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hOutR);

    std::string err;
    while (ReadFile(hErrR, tmp, sizeof(tmp), &got, nullptr) && got > 0)
        err.append(tmp, got);
    CloseHandle(hErrR);
    if (!err.empty()) g_lastAdbErr = err;

    out.assign(data.begin(), data.end());
    return true;
}

// 不带 -s 的 adb 调用（devices / 版本探测等）
static bool RunAdbRaw(const std::wstring& args, std::string& out) {
    std::wstring cmd = L"\"" + g_adbPath + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    HANDLE hR = nullptr, hW = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&hR, &hW, &sa, 0)) return false;
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hW; si.hStdError = hW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hR); CloseHandle(hW); return false;
    }
    CloseHandle(hW);
    std::vector<char> data; char tmp[8192]; DWORD got;
    while (ReadFile(hR, tmp, sizeof(tmp), &got, nullptr) && got > 0)
        data.insert(data.end(), tmp, tmp + got);
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hR);
    size_t n = data.size();
    while (n > 0 && data[n - 1] == 0) n--;
    out.assign(data.begin(), data.begin() + n);
    return true;
}

static bool GetGamePid(int& pid);   // 前置声明，供设备探测使用

// 自动找到 adb.exe：ini 路径失效时扫常见雷电安装位置，最后交给 PATH。
static void ResolveAdbExe() {
    if (GetFileAttributesW(g_adbPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    static const wchar_t* cands[] = {
        L"D:\\leidian\\LDPlayer14\\adb.exe", L"D:\\leidian\\LDPlayer9\\adb.exe",
        L"D:\\leidian\\LDPlayer5\\adb.exe", L"D:\\leidian\\LDPlayer14\\adb.exe",
        L"C:\\leidian\\LDPlayer14\\adb.exe", L"E:\\leidian\\LDPlayer14\\adb.exe",
        L"C:\\Program Files\\ldplayer\\adb.exe", L"C:\\LDPlayer\\adb.exe",
    };
    for (auto p : cands) {
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { g_adbPath = p; return; }
    }
    g_adbPath = L"adb";   // 让 CreateProcess 走 PATH
}

// 自动连上正确的模拟器：重启雷电后 serial 可能变、adbd 可能掉回非 root。
// 遍历所有在线设备，挑出真正跑着 com.tencent.KiHan 的那个，并确保 root。
static bool EnsureAdb(bool force = false) {
    DWORD now = GetTickCount();
    if (!force && g_adbReady && now - g_lastProbe < 30000) return true;
    g_lastProbe = now;
    ResolveAdbExe();

    std::string dev;
    if (!RunAdbRaw(L"devices", dev)) { g_adbReady = false; return false; }
    std::vector<std::wstring> serials;
    std::istringstream is(dev);
    std::string line;
    while (std::getline(is, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos || line.find("device", tab) == std::string::npos) continue;
        std::string s = line.substr(0, tab);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s.find("List") != std::string::npos) continue;
        serials.push_back(std::wstring(s.begin(), s.end()));
    }
    if (serials.empty()) { g_adbReady = false; return false; }

    std::wstring old = g_adbSer;
    std::wstring chosen;
    for (const auto& s : serials) {
        g_adbSer = s;
        int p = 0;
        if (GetGamePid(p)) { chosen = s; break; }
    }
    if (chosen.empty()) { g_adbSer = old; g_adbReady = false; return false; }
    g_adbSer = chosen;

    std::string id, dummy;
    RunAdb(L"shell id", id);
    if (id.find("uid=0") == std::string::npos) {
        RunAdb(L"root", dummy);
        Sleep(1500);
        RunAdb(L"wait-for-device", dummy);
        RunAdb(L"shell id", id);
    }
    g_adbReady = (id.find("uid=0") != std::string::npos);
    return g_adbReady;
}

static void AdbRoot() {
    std::string r;
    RunAdb(L"root", r);
    Sleep(1500);  // adbd 重启为 root 需要一点时间
}

static bool GetGamePid(int& pid) {
    std::string r;
    if (!RunAdb(L"shell pidof com.tencent.KiHan", r)) return false;
    size_t p = r.find_first_of("0123456789");
    if (p == std::string::npos) return false;
    try { pid = std::stoi(r.substr(p)); return true; }
    catch (...) { return false; }
}

// 从当前进程的 maps 里取 libil2cpp.so 加载基址。
// 只有「模块基址 + 模块内偏移」跨重启稳定；托管堆 VA 每次重启都会整体漂移。
static uint64_t FindModuleBase(int pid) {
    std::string s;
    if (!RunAdb(L"shell cat /proc/" + std::to_wstring(pid) + L"/maps", s)) return 0;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("libil2cpp.so") == std::string::npos) continue;
        size_t dash = line.find('-');
        if (dash == std::string::npos) continue;
        try { return std::stoull(line.substr(0, dash), nullptr, 16); }
        catch (...) { return 0; }
    }
    return 0;
}

// 取 guest 内所有 >=1MB 的匿名 rw-p 区（含低地址大块）。
// 绝不写死 VA 窗口：实测模拟器重启后托管堆从 0x7C14… 整体漂到 0x7211…，
// 任何固定地址窗口都会 0 命中。区大小也不设上限（实测有 384MB 区）。
static std::vector<std::pair<uint64_t, uint64_t>> GetRegions(int pid) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    std::string s;
    if (!RunAdb(L"shell cat /proc/" + std::to_wstring(pid) + L"/maps", s)) return out;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string range = line.substr(0, sp);
        size_t sp2 = line.find(' ', sp + 1);
        if (sp2 == std::string::npos) continue;
        std::string perms = line.substr(sp + 1, sp2 - (sp + 1));
        if (perms.size() < 2 || perms[0] != 'r' || perms[1] != 'w') continue;
        std::string rest = line.substr(sp2 + 1);
        if (rest.find('/') != std::string::npos) continue;  // 文件映射跳过
        size_t d = range.find('-');
        if (d == std::string::npos) continue;
        uint64_t a = std::stoull(range.substr(0, d), nullptr, 16);
        uint64_t b = std::stoull(range.substr(d + 1), nullptr, 16);
        uint64_t sz = b - a;
        if (sz >= 0x100000ULL && sz <= 0x80000000ULL)  // 1MB..2GB
            out.push_back({ a, b });
    }
    return out;
}

// ---------------- 列出全堆所有 UI 进度条/遮罩 widget ----------------
// 血量块那个结构（float 值 + 8 零 + 01 00 00 00 + owner + 24B TAIL）是通用进度条描述符。
// 技能冷却的环形遮罩是同一个类 -> 把这些全列出来，其中必有 CD 遮罩。
static bool ReadChunk(uint64_t pageVa, uint32_t pages, std::vector<BYTE>& out);
static void ScanRegion(const std::vector<BYTE>& d, uint64_t base,
                       std::vector<std::pair<float, uint64_t>>& hits);

static void DoWidgets() {
    if (!EnsureAdb(true)) { printf("adb 未就绪\n"); return; }
    int pid = 0;
    if (!GetGamePid(pid)) { printf("找不到游戏进程\n"); return; }
    g_pid = pid;
    auto regs = GetRegions(pid);
    struct Task { uint64_t va; uint32_t pages; };
    std::vector<Task> tasks;
    for (auto& r : regs) {
        for (uint64_t a = r.first; a < r.second; ) {
            uint64_t n = std::min<uint64_t>(r.second - a, (uint64_t)4096 * 0x1000);
            uint32_t pg = (uint32_t)(n / 0x1000);
            if (pg) tasks.push_back({ a, pg });
            a += (uint64_t)pg * 0x1000;
            if (!pg) break;
        }
    }
    printf("区域 %zu 分块 %zu\n", regs.size(), tasks.size());
    CRITICAL_SECTION cs; InitializeCriticalSection(&cs);
    volatile LONG idx = 0;
    std::vector<std::pair<float, uint64_t>> all;
    auto worker = [&]() {
        std::vector<BYTE> d;
        std::vector<std::pair<float, uint64_t>> hits;
        for (;;) {
            LONG i = InterlockedIncrement(&idx) - 1;
            if (i >= (LONG)tasks.size()) break;
            hits.clear();
            if (!ReadChunk(tasks[i].va, tasks[i].pages, d) || d.size() < 0x40) continue;
            ScanRegion(d, tasks[i].va, hits);
            if (!hits.empty()) {
                EnterCriticalSection(&cs);
                all.insert(all.end(), hits.begin(), hits.end());
                LeaveCriticalSection(&cs);
            }
        }
    };
    DWORD t0 = GetTickCount();
    std::vector<std::thread> ths;
    for (int i = 0; i < 6; ++i) ths.emplace_back(worker);
    for (auto& t : ths) t.join();
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    printf("共 %zu 个 widget  (took %lu ms)\n", all.size(), (unsigned long)(GetTickCount() - t0));
    for (auto& h : all) printf("  %012llX  %8.5f\n", (unsigned long long)h.second, h.first);
    DeleteCriticalSection(&cs);
}

// ---------------- 指针回溯：谁指向这段地址 ----------------
// 用途：拿到血量值 VA 后，扫全堆找指向它的 qword，从而定位角色对象本体。
static bool ReadChunk(uint64_t pageVa, uint32_t pages, std::vector<BYTE>& out);

static void DoPtrScan(uint64_t lo, uint64_t hi) {
    if (!EnsureAdb(true)) { printf("adb 未就绪\n"); return; }
    int pid = 0;
    if (!GetGamePid(pid)) { printf("找不到游戏进程\n"); return; }
    g_pid = pid;
    auto regs = GetRegions(pid);
    uint64_t total = 0;
    for (auto& r : regs) total += r.second - r.first;
    printf("区域 %zu 个, 共 %.0f MB\n", regs.size(), total / 1048576.0);
    printf("目标区间 [%012llX, %012llX)\n", (unsigned long long)lo, (unsigned long long)hi);

    struct Task { uint64_t va; uint32_t pages; };
    std::vector<Task> tasks;
    const uint32_t CH = 4096;                      // 16MB / 页
    for (auto& r : regs) {
        for (uint64_t a = r.first; a < r.second; ) {
            uint64_t n = std::min<uint64_t>(r.second - a, (uint64_t)CH * 0x1000);
            uint32_t pg = (uint32_t)(n / 0x1000);
            if (pg) tasks.push_back({ a, pg });
            a += (uint64_t)pg * 0x1000;
            if (!pg) break;
        }
    }
    printf("分块 %zu 个\n", tasks.size());
    CRITICAL_SECTION cs; InitializeCriticalSection(&cs);
    volatile LONG idx = 0;
    int nThreads = 6;
    auto worker = [&]() {
        std::vector<BYTE> d;
        for (;;) {
            LONG i = InterlockedIncrement(&idx) - 1;
            if (i >= (LONG)tasks.size()) break;
            uint64_t va = tasks[i].va;
            if (!ReadChunk(va, tasks[i].pages, d) || d.size() < 8) { continue; }
            std::vector<std::pair<uint64_t, uint64_t>> hit;
            size_t n = d.size();
            for (size_t k = 0; k + 8 <= n; k += 8) {
                uint64_t v = *(const uint64_t*)(d.data() + k);
                if (v >= lo && v < hi) hit.push_back({ va + k, v });
            }
            if (!hit.empty()) {
                EnterCriticalSection(&cs);
                for (auto& h : hit)
                    printf("  指向者 %012llX (偏移 -%llX) -> %012llX\n",
                           (unsigned long long)h.first, (unsigned long long)(h.second - h.first),
                           (unsigned long long)h.second);
                LeaveCriticalSection(&cs);
            }
        }
    };
    DWORD t0 = GetTickCount();
    std::vector<std::thread> ths;
    for (int i = 0; i < nThreads; ++i) ths.emplace_back(worker);
    for (auto& t : ths) t.join();
    printf("完成 %lu ms\n", (unsigned long)(GetTickCount() - t0));
    DeleteCriticalSection(&cs);
}

// 把一段 guest 内存通过 exec-out 直接以字节流取回（绕开 guest 临时文件与 pull）
static bool PullRegion(int pid, uint64_t base, uint64_t size, std::vector<BYTE>& out) {
    uint64_t pages = (base + size) / 4096 - base / 4096;
    if (pages == 0) return false;
    // 注意：exec-out 不走设备端 shell，不能写 2>/dev/null（会被当成 dd 参数而报错）。
    // 用 conv=noerror,sync 让 dd 穿透未映射空洞（填零继续），并截掉末尾汇总文本。
    std::wstring args = L"exec-out toybox dd if=/proc/" + std::to_wstring(pid) +
        L"/mem bs=4096 skip=" + std::to_wstring(base / 4096) +
        L" count=" + std::to_wstring(pages) + L" conv=noerror,sync";
    if (!RunAdbBin(args, out) || out.empty()) return false;
    size_t expected = (size_t)pages * 4096;
    if (out.size() > expected) out.resize(expected);
    return !out.empty();
}

// 血量块尾部标记（+0x38 起），先在 region 里找它，再回校验头部结构
static const BYTE TAIL[24] = {
    0x01,0x00,0x00,0x00, 0x01,0x00,0x00,0x00, 0x02,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00, 0x01,0x00,0x00,0x00, 0x01,0x00,0x00,0x00
};

static void ScanRegion(const std::vector<BYTE>& d, uint64_t base,
                       std::vector<std::pair<float, uint64_t>>& hits) {
    size_t n = d.size();
    if (n < 0x40) return;
    for (size_t i = 0; i + 24 <= n; ++i) {
        bool ok = true;
        for (int k = 0; k < 24; ++k) if (d[i + k] != TAIL[k]) { ok = false; break; }
        if (!ok) continue;
        long long s = (long long)i - 0x38;     // 块起始（血量 float 处）
        if (s < 0) continue;
        size_t ss = (size_t)s;
        if (ss + 0x40 > n) continue;
        bool zero = true;
        for (int k = 4; k < 0x0C; ++k) if (d[ss + k] != 0) { zero = false; break; }
        if (!zero) continue;
        if (!(d[ss + 0x0C] == 0x01 && d[ss + 0x0D] == 0x00 &&
              d[ss + 0x0E] == 0x00 && d[ss + 0x0F] == 0x00)) continue;
        float hp; memcpy(&hp, &d[ss], 4);
        if (hp > 0.0001f && hp <= 1.001f)
            hits.push_back({ hp, base + ss });
    }
}

// ====================== exe 内置自动定位器 ======================
// 不依赖 Python、不依赖任何写死的 VA、不需要用户做任何操作。
// 唯一写死的是「HP 块结构特征」：+00 float hp / +04..0B 全 0 / +0C int 1
// / +38 起 24 字节 TAIL / +10 owner 指针。这个结构只由游戏版本决定。

struct HpHit {
    uint64_t va, owner;
    float    hp;
};
struct Entity {
    uint64_t vaA, vaB, owner;
    uint32_t delta;
    float    a, b;
};

static std::vector<HpHit> g_lastHits;   // 最近一次定位扫到的全部血量块（诊断用）

// 一次 dd 连读 pages 个 4KB 页；允许尾部略短（未映射页 conv=noerror,sync 填零）
// guest 端接 gzip -1 压缩后再传：堆里 97%+ 是零页，实测 64MB 压到 ~0.8MB，
// adb 通道（实测 ~80MB/s）等效带宽提升一个数量级，全量扫描从 30s 级降到 5s 内。
// host 端用 miniz 的 tinfl 做裸 deflate 解压（跳过 gzip 头尾）。
static bool Gunzip(const std::vector<BYTE>& in, size_t expect, std::vector<BYTE>& out) {
    (void)expect;
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return false;
    BYTE flg = in[3];
    size_t p = 10;                                       // 固定头 10 字节
    if (flg & 0x04) { if (p + 2 > in.size()) return false;
        p += 2 + (size_t)in[p] + ((size_t)in[p + 1] << 8); }   // FEXTRA
    if (flg & 0x08) { while (p < in.size() && in[p]) ++p; ++p; }  // FNAME
    if (flg & 0x10) { while (p < in.size() && in[p]) ++p; ++p; }  // FCOMMENT
    if (flg & 0x02) p += 2;                              // FHCRC
    if (p + 8 >= in.size()) return false;                // 尾部还有 CRC32+ISIZE 8 字节
    // 用 zlib 的 inflate（-15 = 裸 deflate）直接解到精确大小的缓冲区：
    // miniz 的 tinfl 只能到 ~70MB/s，zlib 的 inffast 路径实测 300MB/s 以上。
    out.resize(expect);
    z_stream zs{};
    if (inflateInit2(&zs, -15) != Z_OK) return false;
    zs.next_in  = (Bytef*)(in.data() + p);
    zs.avail_in = (uInt)(in.size() - p - 8);
    zs.next_out = out.data();
    zs.avail_out = (uInt)expect;
    int rc = inflate(&zs, Z_FINISH);
    size_t produced = expect - zs.avail_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_BUF_ERROR) return false;
    out.resize(produced);
    return produced > 0;
}

static volatile LONG64 g_tRead = 0, g_tGunzip = 0, g_nChunk = 0;   // 诊断计时

static bool ReadChunk(uint64_t pageVa, uint32_t pages, std::vector<BYTE>& out) {
    DWORD t0 = GetTickCount();
    static const bool noGz = (getenv("HUOYING_NOGZIP") != nullptr);   // 诊断用
    if (pages == 0 || pages > 16384) return false;   // 32MB 上限
    if (noGz) {
        std::wstring plain = L"exec-out toybox dd if=/proc/" + std::to_wstring(g_pid) +
            L"/mem bs=4096 skip=" + std::to_wstring(pageVa / 4096) +
            L" count=" + std::to_wstring(pages) + L" conv=noerror,sync";
        if (!RunAdbBin(plain, out)) return false;
        if (out.size() > (size_t)pages * 0x1000) out.resize((size_t)pages * 0x1000);
        return out.size() >= 0x50;
    }
    // 注意：adb exec-out 直接 exec，不做 shell 解析 —— 管道必须整条包成一个参数
    // 传给设备端 shell，否则 "|" 会被 dd 当成非法参数（实测 dd: bad arg |）。
    std::wstring remote = L"toybox dd if=/proc/" + std::to_wstring(g_pid) +
        L"/mem bs=4096 skip=" + std::to_wstring(pageVa / 4096) +
        L" count=" + std::to_wstring(pages) +
        L" conv=noerror,sync | toybox gzip -1";
    std::wstring args = L"exec-out \"" + remote + L"\"";
    std::vector<BYTE> gz;
    if (!RunAdbBin(args, gz) || gz.empty()) return false;
    DWORD t1 = GetTickCount();
    InterlockedAdd64(&g_tRead, t1 - t0);
    InterlockedIncrement64(&g_nChunk);
    if (gz.size() > 2 && gz[0] == 0x1F && gz[1] == 0x8B) {
        out.resize((size_t)pages * 0x1000);              // 精确大小：dd 已按页补零
        if (!Gunzip(gz, out.size(), out)) {
            // gzip 流坏了：退回原始读取，慢但一定对
            std::wstring plain = L"exec-out toybox dd if=/proc/" + std::to_wstring(g_pid) +
                L"/mem bs=4096 skip=" + std::to_wstring(pageVa / 4096) +
                L" count=" + std::to_wstring(pages) + L" conv=noerror,sync";
            if (!RunAdbBin(plain, out)) return false;
        }
    } else {
        out.swap(gz);                    // 老系统没有 gzip：按原始字节流兜底
    }
    InterlockedAdd64(&g_tGunzip, GetTickCount() - t1);
    size_t want = (size_t)pages * 0x1000;
    if (out.size() > want) out.resize(want);   // 去掉 dd 汇总/尾巴
    if (out.size() < 0x50) return false;
    return true;
}

// 扫描一个映射区。相邻 chunk 之间保留 0x50 字节重叠，
// 保证「血量块正好跨 dd 边界」时不会漏掉一份。
static void ScanArea(uint64_t a, uint64_t b, std::vector<HpHit>& hits,
                     uint64_t& bytes, DWORD deadline) {
    const uint64_t STEP = 0x1000000ULL;   // 16MB / 次
    const size_t   OVL  = 0x50;
    uint64_t p = (a + 0xFFFULL) & ~0xFFFULL;
    uint64_t end = (b + 0xFFFULL) & ~0xFFFULL;
    std::vector<BYTE> prevTail;
    while (p < end && GetTickCount() < deadline) {
        uint64_t n = std::min<uint64_t>(STEP, end - p);
        uint32_t pages = (uint32_t)(n / 0x1000);
        if (!pages) break;
        std::vector<BYTE> d;
        if (!ReadChunk(p, pages, d)) { p += (uint64_t)pages * 0x1000; continue; }
        bytes += d.size();

        std::vector<BYTE> s;
        s.reserve(prevTail.size() + d.size());
        s.insert(s.end(), prevTail.begin(), prevTail.end());
        s.insert(s.end(), d.begin(), d.end());
        uint64_t sbase = p - prevTail.size();
        for (size_t i = 0; i + 24 <= s.size(); ++i) {
            if (memcmp(s.data() + i, TAIL, 24) != 0) continue;
            if (i < 0x38) continue;
            size_t o = i - 0x38;
            if (o + 0x50 > s.size()) continue;
            for (int k = 4; k < 0x0C; ++k) if (s[o + k]) { o = (size_t)-1; break; }
            if (o == (size_t)-1) continue;
            uint32_t one = 0; memcpy(&one, s.data() + o + 0x0C, 4);
            if (one != 1) continue;
            float hp = 0.f; memcpy(&hp, s.data() + o, 4);
            if (!(std::isfinite(hp) && hp >= 0.0f && hp <= 1.001f)) continue;
            uint64_t own = 0; memcpy(&own, s.data() + o + 0x10, 8);
            if (!own) continue;
            hits.push_back({ sbase + o, own, hp });
        }
        size_t keep = std::min<size_t>(OVL, d.size());
        prevTail.assign(d.end() - keep, d.end());
        p += (uint64_t)pages * 0x1000;
    }
}

// 放宽特征扫描：不要求 24 字节 TAIL，只要求
//    float(0..1) + 8 个零字节 + 01 00 00 00 + 非零 owner 指针
// 用来验证「敌方血块是不是尾部特征和我方不同」——严格扫描找不到它就是因为这个。
static void ScanAreaRelax(uint64_t a, uint64_t b, std::vector<HpHit>& hits,
                          uint64_t& bytes, DWORD deadline) {
    const uint64_t STEP = 0x1000000ULL;
    uint64_t p = (a + 0xFFFULL) & ~0xFFFULL;
    uint64_t end = (b + 0xFFFULL) & ~0xFFFULL;
    while (p < end && GetTickCount() < deadline) {
        uint64_t n = std::min<uint64_t>(STEP, end - p);
        uint32_t pages = (uint32_t)(n / 0x1000);
        if (!pages) break;
        std::vector<BYTE> d;
        if (!ReadChunk(p, pages, d)) { p += (uint64_t)pages * 0x1000; continue; }
        bytes += d.size();
        for (size_t o = 0; o + 0x18 <= d.size(); o += 4) {
            float hp = 0.f; memcpy(&hp, d.data() + o, 4);
            if (!(std::isfinite(hp) && hp > 0.0f && hp <= 1.0f)) continue;
            bool zeros = true;
            for (int k = 4; k < 0x0C; ++k) if (d[o + k]) { zeros = false; break; }
            if (!zeros) continue;
            uint32_t one = 0; memcpy(&one, d.data() + o + 0x0C, 4);
            if (one != 1) continue;
            uint64_t own = 0; memcpy(&own, d.data() + o + 0x10, 8);
            if (own < 0x4000000000ULL || own > 0x7FFFFFFFFFFFULL) continue;
            hits.push_back({ p + o, own, hp });
        }
        p += (uint64_t)pages * 0x1000;
    }
}

// ===================== 并行扫描：定位提速 =====================
// 旧版单线程 + 每 16MB 起一个 adb 进程，2.3GB 要起 140+ 个进程，实测 71 秒。
// 现在把区域切成 8MB 切片队列，8 个线程并行消费（adb 服务端支持并发连接），
// 主线程每 120ms 检查一次「够停了没」，够停就广播停止 —— 实战通常 1~3 秒内命中。
static CRITICAL_SECTION g_scanCs;
static std::vector<std::pair<uint64_t, uint64_t>> g_scanQueue;
static size_t               g_scanNext  = 0;
static std::vector<HpHit>   g_scanHits;
static volatile LONG        g_scanStop  = 0;
static volatile LONG64      g_scanBytes = 0;

static void ScanQueueInit() {
    InitializeCriticalSection(&g_scanCs);
}

// 工作线程：从队列取切片 -> 本地扫描 -> 合并回全局
static DWORD WINAPI ScanWorker(LPVOID) {
    std::vector<HpHit> local;
    local.reserve(256);
    DWORD deadline = GetTickCount() + 60000;
    for (;;) {
        if (g_scanStop) break;
        uint64_t a = 0, b = 0;
        EnterCriticalSection(&g_scanCs);
        if (g_scanNext < g_scanQueue.size()) {
            a = g_scanQueue[g_scanNext].first;
            b = g_scanQueue[g_scanNext].second;
            ++g_scanNext;
        }
        LeaveCriticalSection(&g_scanCs);
        if (!a) break;                       // 队列空，收工
        uint64_t bytes = 0;
        ScanArea(a, b, local, bytes, deadline);
        InterlockedAdd64(&g_scanBytes, (LONG64)bytes);
        if (!local.empty()) {
            EnterCriticalSection(&g_scanCs);
            g_scanHits.insert(g_scanHits.end(), local.begin(), local.end());
            LeaveCriticalSection(&g_scanCs);
            local.clear();
        }
        if (GetTickCount() >= deadline) break;
    }
    return 0;
}

// 切片大小：8MB。太小则切片数过多，太大则提前停止时浪费多。
static const uint64_t SCAN_SLICE = 0x1000000ULL;   // 16MB/片：实测 32MB 会让 toybox gzip 截断

// 把已按优先级排序的区域表铺成切片队列（每片向前多带 0x100 字节，
// 保证正好压在切片边界上的血量块不被切没 —— 相邻片重复扫 0x100 无妨，
// BuildEntities 会按 VA 去重）。
static void BuildScanQueue(const std::vector<std::pair<uint64_t, uint64_t>>& regs) {
    g_scanQueue.clear();
    g_scanNext = 0;
    g_scanHits.clear();
    g_scanStop = 0;
    g_scanBytes = 0;
    for (auto& r : regs) {
        uint64_t a = (r.first + 0xFFF) & ~0xFFFULL;
        uint64_t end = (r.second + 0xFFF) & ~0xFFFULL;
        for (uint64_t p = a; p < end; p += SCAN_SLICE) {
            uint64_t s = (p > a + 0x100) ? p - 0x100 : a;   // 带一点前向重叠
            uint64_t e = std::min<uint64_t>(p + SCAN_SLICE, end);
            g_scanQueue.push_back({ s, e });
        }
    }
}

// 把命中按「同 owner + 间距 0x100..0x8000」配成同一实体的两份血量。
// 实测同一实体的两份血量间距随角色变化（0x900 / 0x36C0 / 0x46C0 / 0x56C0），
// 所以绝不能写死；owner 相同是配对依据，不同实体也可能共享同一战斗 owner。
static std::vector<Entity> BuildEntities(std::vector<HpHit> h) {
    std::sort(h.begin(), h.end(), [](const HpHit& x, const HpHit& y) {
        return x.va < y.va;
    });
    h.erase(std::unique(h.begin(), h.end(),
        [](const HpHit& x, const HpHit& y) { return x.va == y.va; }), h.end());
    std::stable_sort(h.begin(), h.end(), [](const HpHit& x, const HpHit& y) {
        if (x.owner != y.owner) return x.owner < y.owner;
        return x.va < y.va;
    });
    std::vector<Entity> out;
    size_t i = 0;
    while (i < h.size()) {
        size_t j = i + 1;
        while (j < h.size() && h[j].owner == h[i].owner) ++j;
        for (size_t k = i; k + 1 < j;) {
            uint64_t d = h[k + 1].va - h[k].va;
            if (d >= 0x100 && d <= 0x8000) {
                out.push_back({ h[k].va, h[k + 1].va, h[k].owner,
                                (uint32_t)d, h[k].hp, h[k + 1].hp });
                k += 2;
            } else ++k;
        }
        i = j;
    }
    // 参战对象（血量已扣，偏离 1.0）优先；都满血时按低 VA 优先
    std::sort(out.begin(), out.end(), [](const Entity& x, const Entity& y) {
        float dx = 1.0f - std::min(x.a, x.b), dy = 1.0f - std::min(y.a, y.b);
        if (std::fabs(dx - dy) > 0.02f) return dx > dy;
        return x.vaA < y.vaA;
    });
    return out;
}

// 区域优先级：缓存 VA 所在区 > 高地址 8~64MB 堆（实证命中区） > 其它高地址
// > 低地址 ashmem。先扫小区能最快凑齐双方，避免一上来读 384MB 大区。
static int RegionScore(uint64_t a, uint64_t b, uint64_t hint) {
    uint64_t sz = b - a;
    int s = 0;
    if (hint >= a && hint < b) s += 10000;
    if (a >= 0x700000000000ULL) s += 300;
    else if (a >= 0x400000000000ULL) s += 100;
    // 大堆优先：IL2CPP 的托管堆/战斗对象都在 128MB 以上的匿名区（实测 273MB 区命中），
    // 先把这些区扫掉，通常 2~4 秒就能定位，不必再碰剩下 700MB 小堆。
    if (sz >= 0x8000000ULL && a >= 0x700000000000ULL) s += 600;
    if (sz >= 0x800000ULL && sz <= 0x4000000ULL) s += 500;
    else if (sz > 0x4000000ULL) s += 50;
    return s;
}

// ---------------- 血条实测（位置自适应，绝不写死 y） ----------------
// 教训（2026-11-09）：旧版把取样窗口写死成 y 0.106~0.124H。这套坐标是在 1962x1121 的
// 截图上标出来的，换到 1080p 后血条实际在 0.077~0.088H，取样整整低了 30 行 —— 量到的是
// 血条下面的技能/替身图标，于是「我方条 34.9%」是假的，归属被它带反（敌方当成我方）。
// 现在：先在上半屏找「橙色血条行带」，再在带内量填充宽度，位置、分辨率都不再依赖标定。
struct Shot {
    std::vector<BYTE> px;
    int W = 0, H = 0, stride = 0;
    bool ok = false;
    const BYTE* at(int x, int y) const { return px.data() + (size_t)y * stride + (size_t)x * 4; }
};

static bool IsHpOrange(BYTE b, BYTE g, BYTE r) {
    return r > 200 && g > 55 && g < 150 && b < 120 && r > g * 1.6f;   // 实测血条橙 (254,82,22)
}

// 截一次设备画面并解码成 32bpp（BGRA），供所有像素分析共用
static bool CaptureShot(Shot& s) {
    std::vector<BYTE> png;
    if (!RunAdbBin(L"exec-out screencap -p", png)) return false;
    for (size_t i = 0; i + 4 <= png.size(); ++i) {   // 去掉 adb 附带的尾巴
        if (png[i] == 0x49 && png[i + 1] == 0x45 && png[i + 2] == 0x4E && png[i + 3] == 0x44) {
            png.resize(i + 8);
            break;
        }
    }
    if (png.size() < 8) return false;

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!hg) return false;
    void* pm = GlobalLock(hg);
    memcpy(pm, png.data(), png.size());
    GlobalUnlock(hg);

    IStream* st = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &st))) { GlobalFree(hg); return false; }
    Bitmap* bmp = Bitmap::FromStream(st);
    if (!bmp || bmp->GetLastStatus() != Ok) {
        if (bmp) delete bmp;
        st->Release();
        return false;
    }
    st->Release();

    s.W = (int)bmp->GetWidth(); s.H = (int)bmp->GetHeight();
    BitmapData bd{};
    Rect rc(0, 0, s.W, s.H);
    if (bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
        delete bmp; return false;
    }
    s.stride = (int)bd.Stride;
    s.px.resize((size_t)s.stride * s.H);
    memcpy(s.px.data(), bd.Scan0, s.px.size());
    bmp->UnlockBits(&bd);
    delete bmp;
    s.ok = true;
    return true;
}

// 在上半屏找血条行带：取「橙色像素最多」的连续行区间，再取带内橙色的横向范围。
// right=false 找左上(我方)条，right=true 找右上(敌方)条。
static bool FindBarBand(const Shot& s, bool right, int& y0, int& y1, int& xa, int& xb,
                        int* outW = nullptr) {
    if (!s.ok) return false;
    const int X0 = (int)(s.W * (right ? 0.50f : 0.02f));
    const int X1 = (int)(s.W * (right ? 0.98f : 0.48f));
    const int Y0 = (int)(s.H * 0.010f), Y1 = (int)(s.H * 0.240f);
    if (X1 - X0 < 40 || Y1 - Y0 < 8) return false;

    std::vector<int> cnt((size_t)(Y1 - Y0), 0);
    for (int y = Y0; y < Y1; ++y) {
        const BYTE* row = s.at(0, y);
        int c = 0;
        for (int x = X0; x < X1; ++x)
            if (IsHpOrange(row[x * 4], row[x * 4 + 1], row[x * 4 + 2])) ++c;
        cnt[(size_t)(y - Y0)] = c;
    }
    int thr = std::max(10, (X1 - X0) / 50);      // 至少占宽 2%，挡掉小图标
    int bs = -1, be = -1; long bestSum = -1;
    for (int i = 0; i < (int)cnt.size();) {
        if (cnt[(size_t)i] < thr) { ++i; continue; }
        int j = i; long sum = 0;
        while (j < (int)cnt.size() && cnt[(size_t)j] >= thr) { sum += cnt[(size_t)j]; ++j; }
        if (sum > bestSum) { bestSum = sum; bs = i; be = j - 1; }
        i = j;
    }
    if (bs < 0) return false;
    y0 = Y0 + bs; y1 = Y0 + be + 1;

    std::vector<int> lo, hi, wd;                 // 每行「最长连续橙段」的左右端与宽度
    for (int y = y0; y < y1; ++y) {
        const BYTE* row = s.at(0, y);
        // 必须取最长的连续段：右上血条右边紧接着角色头像的金色描边，
        // 用「最左/最右橙点」会把头像框也算进来（实测把 31% 量成了 58%）。
        int bs = -1, be = -1, runS = -1, prev = -10;
        for (int x = X0; x < X1; ++x) {
            if (IsHpOrange(row[x * 4], row[x * 4 + 1], row[x * 4 + 2])) {
                if (runS < 0 || x - prev > 4) runS = x;      // 允许 4px 断口（描边/抗锯齿）
                prev = x;
                if (x - runS > be - bs) { bs = runS; be = x; }
            }
        }
        if (bs >= 0 && be - bs >= 12) { lo.push_back(bs); hi.push_back(be); wd.push_back(be - bs + 1); }
    }
    if (lo.empty()) return false;
    std::sort(lo.begin(), lo.end());
    std::sort(hi.begin(), hi.end());
    xa = lo[lo.size() / 2];
    xb = hi[hi.size() / 2];
    // 填充量取「段宽」的 70 分位：血条两端是斜角，逐行取中位数会把满血量成 96%，
    // 满血被判成"掉了点血"，反而容易和假块混淆。
    std::sort(wd.begin(), wd.end());
    if (outW) *outW = wd[wd.size() * 7 / 10];
    return (xb - xa) >= 20;
}

// 整条血槽宽度（占屏宽比例）：1080p 实测左右槽都是 347px/1920 = 0.1807。
// 填充比例 = 橙色段宽度 / 槽宽（左条锚左端、右条锚右端，段宽就是填充量）。
static const float BAR_SLOT_FRAC = 0.1765f;   // 339px/1920：满血正好量成 100%
static float MeasureBarEx(bool right, int* outY = nullptr, int* outX = nullptr) {
    Shot s;
    if (!CaptureShot(s)) return -1.f;
    int y0 = 0, y1 = 0, xa = 0, xb = 0, wf = 0;
    if (!FindBarBand(s, right, y0, y1, xa, xb, &wf)) return -1.f;
    float slot = BAR_SLOT_FRAC * (float)s.W;
    float fill = (float)wf / std::max(slot, 1.f);
    if (outY) *outY = y0;
    if (outX) *outX = xa;
    return std::clamp(fill, 0.f, 1.f);
}

static float MeasureBar()  { return MeasureBarEx(false); }   // 左上 = 我方
static float MeasureBarR() { return MeasureBarEx(true);  }   // 右上 = 敌方（从右往左填）

// 一次截屏同时拿到：左/右血条填充比例 + 我方血条矩形（省掉重复 screencap）
static bool ProbeBarsOnce(float& barL, float& barR,
                          float* rx = nullptr, float* ry = nullptr,
                          float* rw = nullptr, float* rh = nullptr) {
    Shot s;
    if (!CaptureShot(s)) return false;
    barL = barR = -1.f;

    int y0 = 0, y1 = 0, xa = 0, xb = 0, wf = 0;
    if (FindBarBand(s, false, y0, y1, xa, xb, &wf)) {
        barL = std::clamp((float)wf / std::max(BAR_SLOT_FRAC * s.W, 1.f), 0.f, 1.f);
        if (rx && y1 - y0 >= 5 && xa > (int)(s.W * 0.01f)) {
            *rx = (float)xa / (float)s.W;
            *ry = (float)y0 / (float)s.H;
            *rw = BAR_SLOT_FRAC;
            *rh = (float)(y1 - y0) / (float)s.H;
        }
    }
    int y0r = 0, y1r = 0, xar = 0, xbr = 0, wfr = 0;
    if (FindBarBand(s, true, y0r, y1r, xar, xbr, &wfr))
        barR = std::clamp((float)wfr / std::max(BAR_SLOT_FRAC * s.W, 1.f), 0.f, 1.f);
    return true;
}

// 把叠加层贴到游戏血条原位（老版本写死 y=0.16057H，1080p 下低了 90 像素，飘在战场上）
static void ApplyBarRect(float rx, float ry, float rw, float rh) {
    if (std::fabs(rx - (float)g_view.x) < 0.004f && std::fabs(ry - (float)g_view.y) < 0.004f &&
        std::fabs(rw - (float)g_view.w) < 0.004f && std::fabs(rh - (float)g_view.h) < 0.002f)
        return;
    g_view.x = rx; g_view.y = ry; g_view.w = rw; g_view.h = rh;
    wchar_t buf[32];
    auto put = [&](const wchar_t* k, double v) {
        swprintf_s(buf, L"%.6f", v);
        WritePrivateProfileStringW(L"view", k, buf, g_iniPath.c_str());
    };
    put(L"x", g_view.x); put(L"y", g_view.y); put(L"w", g_view.w); put(L"h", g_view.h);
    printf("[视图] 叠加层对齐游戏血条: x=%.4f y=%.4f w=%.4f h=%.4f\n", rx, ry, rw, rh);
}

// 诊断：解析截图，按连续红色段打印左右血条的精确像素位置（校准 MeasureBar 坐标）
static void DebugBar(const wchar_t* path) {
    Bitmap* bmp = Bitmap::FromFile(path);
    if (!bmp || bmp->GetLastStatus() != Ok) { printf("加载失败: %ls\n", path); return; }
    UINT W = bmp->GetWidth(), H = bmp->GetHeight();
    BitmapData bd{}; Rect rc(0, 0, W, H);
    if (bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
        delete bmp; printf("LockBits 失败\n"); return;
    }
    printf("图像 %ux%u\n", W, H);
    auto scan = [&](const char* tag, float fy0, float fy1, float fx0, float fx1) {
        printf("-- %s --\n", tag);
        int rows = 0;
        for (UINT y = (UINT)(H * fy0); y < (UINT)(H * fy1) && rows < 22; y += 2) {
            BYTE* row = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
            int run = -1, bestS = -1, bestE = -1, bestLen = 0;
            for (UINT x = (UINT)(W * fx0); x < (UINT)(W * fx1); ++x) {
                BYTE b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
                bool red = (r > 200 && g > 55 && g < 150 && b < 120 && r > g * 1.6f);   // 橙黄血条
                if (red) { if (run < 0) run = (int)x; }
                else if (run >= 0) {
                    if ((int)x - run > bestLen) { bestLen = (int)x - run; bestS = run; bestE = (int)x - 1; }
                    run = -1;
                }
            }
            if (run >= 0 && (int)(W * fx1) - run > bestLen) { bestLen = (int)(W * fx1) - run; bestS = run; bestE = (int)(W * fx1) - 1; }
            if (bestLen >= 20) {
                printf("y=%4u  最长红段 %4d..%4d  len=%4d  [%.3fW..%.3fW]\n",
                       y, bestS, bestE, bestLen, bestS / (double)W, bestE / (double)W);
                ++rows;
            }
        }
    };
    scan("左上(我方)", 0.09f, 0.145f, 0.015f, 0.45f);
    scan("右上(敌方)", 0.09f, 0.145f, 0.58f, 0.985f);

    // 宽阈值：把「暗红填充」也算进来，用来量敌方血条真实边界
    printf("-- 右半宽阈值(reddish: r>90 且 r>1.5g 且 r>1.4b) --\n");
    for (UINT y = (UINT)(H * 0.095f); y < (UINT)(H * 0.135f); y += 2) {
        BYTE* row = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
        int first = -1, last = -1, cnt = 0;
        for (UINT x = (UINT)(W * 0.55f); x < W; ++x) {
            BYTE b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
            if (r > 90 && r > 1.5f * g && r > 1.4f * b) {
                if (first < 0) first = (int)x;
                last = (int)x; ++cnt;
            }
        }
        if (cnt > 10)
            printf("y=%4u  x=%4d..%4d  cnt=%4d  [%.3fW..%.3fW]\n",
                   y, first, last, cnt, first / (double)W, last / (double)W);
    }
    {
        UINT y = (UINT)(H * 0.114f);
        BYTE* row = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
        printf("-- 敌方血条行 y=%u 色值 (R,G,B) --\n", y);
        for (UINT x = (UINT)(W * 0.655f); x < (UINT)(W * 0.935f); x += 8) {
            printf("%4u:(%3u,%3u,%3u) ", x, row[x * 4 + 2], row[x * 4 + 1], row[x * 4]);
            if (((x / 8) % 8) == 7) printf("\n");
        }
        printf("\n");
    }
    // 血条横截面色值：亮红(填充) -> 暗红(空槽) 的分界一目了然
    for (float fy : { 0.112f, 0.116f }) {
        UINT y = (UINT)(H * fy);
        BYTE* row = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
        printf("-- 色值采样 y=%u --\n", y);
        for (UINT x = (UINT)(W * 0.07f); x < (UINT)(W * 0.32f); x += 12) {
            printf("%4u:(%3u,%3u,%3u) ", x, row[x * 4 + 2], row[x * 4 + 1], row[x * 4]);
            if ((x / 12) % 6 == 5) printf("\n");
        }
        printf("\n");
    }
    bmp->UnlockBits(&bd);
    delete bmp;
}

// 截设备原始画面（与 MeasureBar 同源，坐标比例才一致）并分析血条
static void DebugBarScreen() {
    EnsureAdb(true);
    int pid = 0;
    if (GetGamePid(pid)) g_pid = pid;
    std::vector<BYTE> png;
    if (!RunAdbBin(L"exec-out screencap -p", png)) { printf("screencap 失败\n"); return; }
    for (size_t i = 0; i + 4 <= png.size(); ++i) {
        if (png[i] == 0x49 && png[i + 1] == 0x45 && png[i + 2] == 0x4E && png[i + 3] == 0x44) {
            png.resize(i + 8); break;
        }
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"screen_now.png", L"wb") == 0 && f) {
        fwrite(png.data(), 1, png.size(), f);
        fclose(f);
    }
    printf("saved screen_now.png (%zu bytes)\n", png.size());
    DebugBar(L"screen_now.png");
}

// 快速读取某个 guest 地址的 float（单次 exec-out，~200ms）
static bool FastReadHp(uint64_t va, float& out) {
    uint64_t page = va & ~0xFFFULL;
    std::wstring args = L"exec-out toybox dd if=/proc/" + std::to_wstring(g_pid) +
        L"/mem bs=4096 skip=" + std::to_wstring(page / 4096) + L" count=1 conv=noerror,sync";
    std::vector<BYTE> buf;
    if (!RunAdbBin(args, buf) || buf.size() < 0x1000) return false;
    if (buf.size() > 0x1000) buf.resize(0x1000);  // 去掉 dd 汇总文本
    uint32_t off = (uint32_t)(va & 0xFFF);
    memcpy(&out, buf.data() + off, 4);
    return true;
}

// ------------------------------------------------------------------ 直锚 / 静态根链直读（默认路径，零扫描）
// 每次刷新只做 5 次单页 exec-out dd 定点读，不遍历任何内存区、不搜特征码。
static bool ReadGuestQword(uint64_t va, uint64_t& out) {
    uint64_t page = va & ~0xFFFULL;
    uint32_t off = (uint32_t)(va & 0xFFF);
    if (off + 8 > 0x1000) return false;
    std::wstring args = L"exec-out toybox dd if=/proc/" + std::to_wstring(g_pid) +
        L"/mem bs=4096 skip=" + std::to_wstring(page / 4096) + L" count=1 conv=noerror,sync";
    std::vector<BYTE> buf;
    if (!RunAdbBin(args, buf) || buf.size() < 0x1000) return false;
    if (buf.size() > 0x1000) buf.resize(0x1000);   // 去掉 dd 汇总文本
    memcpy(&out, buf.data() + off, 8);
    return true;
}

// 把配置里的链根换算成当前进程的绝对 VA：mod 形式自动加上重定位后的模块基址。
static uint64_t SideRoot(const SideState& S) {
    return S.modRoot ? (g_modBase + S.root) : S.root;
}

// 解引用一条链到「值A 所在的块基址」。级数由 nOff 决定，双方各自独立。
static bool ResolveSide(uint64_t& pOut, const SideState& S) {
    uint64_t root = SideRoot(S);
    if (!root) return false;
    uint64_t p = 0;
    if (!ReadGuestQword(root, p) || !p) return false;
    for (int i = 0; i < S.nOff - 1; ++i) {
        uint64_t nxt = 0;
        if (!ReadGuestQword(p + S.off[i], nxt) || !nxt) return false;
        p = nxt;
    }
    pOut = p;
    return true;
}

// 单点读某条链的值A（诊断用）
static bool ReadHpByChain(float& hp, uint64_t* pHpVa = nullptr) {
    uint64_t p = 0;
    if (!ResolveSide(p, g_sides[0])) return false;
    uint64_t hpVa = p + g_sides[0].off[g_sides[0].nOff - 1];
    float v = 0.f;
    if (!FastReadHp(hpVa, v) || !(v > 0.0001f && v <= 1.001f)) return false;
    hp = v;
    if (pHpVa) *pHpVa = hpVa;
    return true;
}

// ------------------------------------------------------------------ 同一实体两份血量（零全内存扫描）
// 值A = va 锚 / 链末级 + off(n-1)；值B = 值A + pair。两块 owner 相同、TAIL 一致。
// 间距可能是 0x900 或 0x46C0（跨多页），所以稳态刷新一次 dd 连读所需页数即可同时取两值。

// 一次 dd 连读 pages 个 4KB 页（值B 可能与值A 相隔 0x46C0，跨多页）。
static bool ReadGuestPage(uint64_t pageVa, std::vector<BYTE>& buf, uint32_t pages = 1) {
    if (pages < 1) pages = 1;
    if (pages > 16) pages = 16;                 // 上限 64KB，防配置写错拉爆 dd
    std::wstring args = L"exec-out toybox dd if=/proc/" + std::to_wstring(g_pid) +
        L"/mem bs=4096 skip=" + std::to_wstring(pageVa / 4096) +
        L" count=" + std::to_wstring(pages) + L" conv=noerror,sync";
    if (!RunAdbBin(args, buf)) return false;
    size_t want = (size_t)pages * 0x1000;
    if (buf.size() > want) buf.resize(want);   // 去掉 dd 汇总文本
    return buf.size() >= want;
}
static uint32_t PagesFor(size_t off, size_t span) {
    uint32_t p = (uint32_t)((off + span + 0xFFF) / 0x1000);
    return p < 1 ? 1 : (p > 16 ? 16 : p);
}

// 血量块形状：+00 float hp / +04..+0B 全 0 / +0C int 1 / +38 起 24 字节 TAIL
static bool HpBlockShape(const BYTE* buf, size_t off, size_t avail) {
    if (off + 0x50 > avail) return false;
    if (memcmp(buf + off + 0x38, TAIL, 24) != 0) return false;
    for (int k = 4; k < 0x0C; ++k) if (buf[off + k]) return false;
    uint32_t one = 0; memcpy(&one, buf + off + 0x0C, 4);
    return one == 1;
}
static uint64_t OwnerAt(const BYTE* buf, size_t off) {
    uint64_t v = 0; memcpy(&v, buf + off + 0x10, 8); return v;
}
static float FloatAt(const BYTE* buf, size_t off) {
    float v = 0.f; memcpy(&v, buf + off, 4); return v;
}
// 血量 0 是合法状态（被击败/血空），只要形状与 owner 校验通过就接受。
// 早期版本要求 >0.0001，导致血空时整条链被误判为失效。
static bool HpRange(float v) {
    return std::isfinite(v) && v >= 0.0f && v <= 1.001f;
}

// 完整解引用：走本方链 -> 值A -> 找同 owner 的值B -> 写缓存。
// 找 B 时先直接试配置的 pair 间距；不成就在 A 所在页 + 相邻页（共 8KB）内局部定位，
// 这不是全内存扫描，只是对同一实体另一份拷贝的确认。
static bool ReadSideFull(SideState& S) {
    uint64_t vaA = 0;
    bool byChain = false;
    // 【进程内稳定锚优先】战斗里 il2cpp 链中间对象会重建：静态根链每几秒就可能
    // 串到别的实体或断掉，血条于是在 100% / 0% / 别人的血量之间来回跳（"蹦迪"）。
    // 血块 VA 在同一进程内固定，所以运行时优先按 va 直读；链只在 va 失效
    //（重启 / 换号 / 换角色）时用于重定位。
    if (S.directVa) {
        std::vector<BYTE> probe;
        uint32_t po = (uint32_t)(S.directVa & 0xFFF);
        if (ReadGuestPage(S.directVa & ~0xFFFULL, probe) &&
            HpBlockShape(probe.data(), po, probe.size()))
            vaA = S.directVa;
        // 直锚失效 = 换号 / 重启 / 换角色。不要用会串台的长链硬读，
        // 直接判失效，由 exe 内置定位器重新找 VA 并写回 address.cache。
        if (!vaA) return false;
    }
    if (!vaA) {
        uint64_t p = 0;
        if (!ResolveSide(p, S)) return false;
        vaA = p + S.off[S.nOff - 1];
        byChain = true;
    }

    // 防串台：只有「走链」且缓存仍合法、目标却变了时才拒绝切换目标。
    if (byChain && S.ready && vaA != S.vaA) {
        std::vector<BYTE> old;
        if (ReadGuestPage(S.pairPage, old) &&
            HpBlockShape(old.data(), S.pairOffA, old.size()) &&
            OwnerAt(old.data(), S.pairOffA) == S.pairOwner)
            return false;   // 交给 ReadSideFast 用缓存继续读
    }

    uint64_t page = vaA & ~0xFFFULL;
    uint32_t offA = (uint32_t)(vaA & 0xFFF);
    std::vector<BYTE> d;
    // 一次连读覆盖 值A .. 值B 所需的全部页。值B 优先用定位器直接给出的 VA：
    // 实测同一实体两路血量的 owner 字段并不相同，靠 owner 找 B 会失败。
    size_t need = (size_t)S.pairDelta + 0x50;
    if (S.directVaB > vaA) need = (size_t)(S.directVaB - vaA) + 0x50;
    if (!ReadGuestPage(page, d, PagesFor(offA, need)))
        return false;
    if (!HpBlockShape(d.data(), offA, d.size())) return false;
    uint64_t owner = OwnerAt(d.data(), offA);
    float a = FloatAt(d.data(), offA);
    if (!HpRange(a)) return false;

    auto isB = [&](size_t o) {
        return o != offA && HpBlockShape(d.data(), o, d.size()) &&
               HpRange(FloatAt(d.data(), o));
    };
    size_t offB = 0;
    if (S.directVaB && S.directVaB >= page &&
        S.directVaB - page + 0x50 <= d.size()) {
        size_t o = (size_t)(S.directVaB - page);
        if (isB(o)) offB = o;
    }
    if (!offB) {
        offB = offA + S.pairDelta;
        if (!isB(offB)) {
            offB = 0;
            size_t bestD = (size_t)-1;
            for (size_t o = 4; o + 0x50 <= d.size(); o += 4) {
                if (!isB(o)) continue;
                size_t dd = o > offA ? o - offA : offA - o;
                if (dd < bestD) { bestD = dd; offB = o; }   // 取离值A最近的一份
            }
            if (!offB) {
                // 非战斗 / 该实体只分配了一份血量时，值B 还不存在。
                // 退化成单值（A、B 同源）让血条照常显示，进战斗后自动重定位会补上第二份。
                offB = offA;
            }
        }
    }

    S.a = a;
    S.b = FloatAt(d.data(), offB);
    S.vaA = vaA;
    S.vaB = vaA + (uint32_t)(offB - offA);
    S.owner = owner;
    S.delta = (uint32_t)(offB - offA);
    S.pairPage  = page;
    S.pairOffA  = offA;
    S.pairOffB  = (uint32_t)offB;
    S.pairOwner = owner;
    // 不覆盖 S.pairDelta：它来自 ini 配置，退化成单值时 delta=0，
    // 下一轮仍要按配置的 +0x900 先试，才能在新对象上找回值B。
    S.ready = true;
    S.ok = true;
    return true;
}

// 稳态刷新：只读缓存页（必要时加下一页），一次 dd 同时取两值并校验形状/owner。
static bool ReadSideFast(SideState& S) {
    if (!S.ready) return false;
    std::vector<BYTE> d;
    uint32_t pages = PagesFor(std::max(S.pairOffA, S.pairOffB), 0x50);
    if (!ReadGuestPage(S.pairPage, d, pages)) return false;
    size_t avail = d.size();
    if (!HpBlockShape(d.data(), S.pairOffA, avail) ||
        !HpBlockShape(d.data(), S.pairOffB, avail)) return false;
    // 只校验值A 的 owner：实测同一实体两路血量的 owner 字段可以不同
    if (OwnerAt(d.data(), S.pairOffA) != S.pairOwner) return false;
    float a = FloatAt(d.data(), S.pairOffA), b = FloatAt(d.data(), S.pairOffB);
    if (!HpRange(a) || !HpRange(b)) return false;
    S.a = a; S.b = b;
    S.vaA = S.pairPage + S.pairOffA;
    S.vaB = S.pairPage + S.pairOffB;
    S.owner = S.pairOwner;
    S.ok = true;
    return true;
}

// 扣血瞬间哪一路先变就给谁记一次；稳态两值相等时同时变化，不计数。
static void NoteSideLead(SideState& S) {
    if (!S.init) { S.lastA = S.a; S.lastB = S.b; S.init = true; return; }
    bool chA = std::fabs(S.a - S.lastA) > 1e-4f;
    bool chB = std::fabs(S.b - S.lastB) > 1e-4f;
    if (chA && !chB) S.leadA++;
    if (chB && !chA) S.leadB++;
    S.lastA = S.a; S.lastB = S.b;
}

// ---------------- 地址缓存：让「第二次打开」秒显示 ----------------
static std::wstring CachePath() { return g_exeDir + L"\\address.cache"; }

static void LoadCache() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, CachePath().c_str(), L"rt") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        unsigned long long v = 0;
        if (sscanf_s(line, "%63[^=]=%llX", key, (unsigned)sizeof(key), &v) != 2) continue;
        if      (!_stricmp(key, "pid"))         g_cachePid = (int)v;
        else if (!_stricmp(key, "self"))        g_cacheVa[0] = v;
        else if (!_stricmp(key, "self_b"))      g_cacheVaB[0] = v;
        else if (!_stricmp(key, "self_pair"))   g_cachePair[0] = (uint32_t)v;
        else if (!_stricmp(key, "enemy"))       g_cacheVa[1] = v;
        else if (!_stricmp(key, "enemy_b"))     g_cacheVaB[1] = v;
        else if (!_stricmp(key, "enemy_pair"))  g_cachePair[1] = (uint32_t)v;
    }
    fclose(f);
}

static void SaveCache() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, CachePath().c_str(), L"wt") != 0 || !f) return;
    fprintf(f, "; 由 exe 自动写入，勿手工编辑\n"
               "pid=%X\nself=%llX\nself_b=%llX\nself_pair=%X\n"
               "enemy=%llX\nenemy_b=%llX\nenemy_pair=%X\n",
            (unsigned)g_cachePid,
            (unsigned long long)g_cacheVa[0], (unsigned long long)g_cacheVaB[0], g_cachePair[0],
            (unsigned long long)g_cacheVa[1], (unsigned long long)g_cacheVaB[1], g_cachePair[1]);
    fclose(f);
}

// 缓存里的 VA 只是「优先试一下」的候选，形状校验不过就自动重定位。
static void ApplyCache() {
    for (int k = 0; k < 2; ++k) {
        if (!g_cacheVa[k]) continue;
        g_sides[k].directVa  = g_cacheVa[k];
        g_sides[k].directVaB = g_cacheVaB[k];
        g_sides[k].pairDelta = g_cachePair[k] ? g_cachePair[k] : 0x900;
    }
}

// 值A 是「结算后」的值：练习场 / 刚开局还没结算时它是 0，按 F1 显示会像坏了。
// 这种情况默认切到值B（实时同步）；用户随时可按 F1 切回来。
static void AutoPickMechIfDead() {
    EnterCriticalSection(&g_cs);
    if (g_sides[0].ok && g_sides[0].a < 0.001f && g_sides[0].b > 0.01f)
        g_mech = 1;
    LeaveCriticalSection(&g_cs);
}

// ---------------- 自动定位主体 ----------------
// 换号 / 换角色 / 重启游戏 / 重启模拟器 后由扫描线程自动调用，用户全程零操作。
static bool AutoLocate() {
    InterlockedExchange(&g_locState, LOC_BUSY);
    InterlockedExchange(&g_locProgress, 0);
    {
        EnterCriticalSection(&g_cs);
        g_locNote = L"正在自动定位血量地址…";
        LeaveCriticalSection(&g_cs);
    }

    auto fail = [](const wchar_t* why) -> bool {
        EnterCriticalSection(&g_cs);
        g_locNote = why;
        LeaveCriticalSection(&g_cs);
        InterlockedExchange(&g_locState, LOC_WAIT);
        return false;
    };

    if (!EnsureAdb(true)) return fail(L"等待模拟器连接…（程序会自动重连）");
    int pid = 0;
    if (!GetGamePid(pid)) return fail(L"等待游戏启动…（程序会自动接管）");
    g_pid = pid;
    if (!g_modBase || pid != g_basePid) {
        uint64_t b = FindModuleBase(pid);
        if (b) { g_modBase = b; g_basePid = pid; }
    }

    auto regs = GetRegions(pid);
    if (regs.empty()) return fail(L"读取内存映射失败，等待重试…");

    // 先测我方血条比例。它既是归属依据，也决定「扫到什么才算找到」：
    // 只看「有两个成对块」就停，会停在满血假块上（实测踩过）。
    // 顺手用实测到的血条矩形把叠加层贴回游戏血条原位（一次截屏全拿到）。
    {
        float rx = 0, ry = 0, rw = 0, rh = 0;
        if (ProbeBarsOnce(g_barL, g_barR, &rx, &ry, &rw, &rh)) ApplyBarRect(rx, ry, rw, rh);
    }
    bool selfBarOk = (g_barL >= 0.02f && g_barL <= 0.985f);

    uint64_t hint = g_cacheVa[0] ? g_cacheVa[0] : g_cacheVa[1];
    std::sort(regs.begin(), regs.end(), [&](const auto& x, const auto& y) {
        int sx = RegionScore(x.first, x.second, hint);
        int sy = RegionScore(y.first, y.second, hint);
        if (sx != sy) return sx > sy;
        return (x.second - x.first) < (y.second - y.first);
    });

    DWORD deadline = GetTickCount() + 60000;
    BuildScanQueue(regs);
    uint64_t total = 0;
    for (auto& s : g_scanQueue) total += s.second - s.first;
    if (total > 0xC0000000ULL) total = 0xC0000000ULL;   // 进度条上限 3GB

    int nThreads = (int)std::thread::hardware_concurrency();
    nThreads = std::max(4, std::min(8, nThreads ? nThreads : 6));
    std::vector<HANDLE> ths;
    for (int i = 0; i < nThreads; ++i) {
        HANDLE h = CreateThread(nullptr, 0, ScanWorker, nullptr, 0, nullptr);
        if (h) ths.push_back(h);
    }

    std::vector<HpHit> hits;
    std::vector<Entity> ents;
    while (!ths.empty()) {
        // 每 120ms 汇总一次进度并检查「够停了没」；够停就广播停止
        if (WaitForMultipleObjects((DWORD)ths.size(), ths.data(), TRUE, 120) == WAIT_OBJECT_0)
            break;                        // 全部工作线程自然收工
        EnterCriticalSection(&g_scanCs);
        hits = g_scanHits;
        LeaveCriticalSection(&g_scanCs);
        int pr = (int)std::min<uint64_t>(99, (uint64_t)g_scanBytes * 100 / std::max<uint64_t>(total, 1));
        InterlockedExchange(&g_locProgress, pr);
        {
            wchar_t msg[96];
            swprintf_s(msg, L"正在自动定位… %d%%（并行扫描中，无需任何操作）", pr);
            EnterCriticalSection(&g_cs);
            g_locNote = msg;
            LeaveCriticalSection(&g_cs);
        }
        if (!g_deepScan) {                // 诊断模式：扫完所有切片再停
            bool stop = (GetTickCount() >= deadline);
            if (selfBarOk) {
                // 找到与左血条吻合的块、且还有敌方候选（其它块 >=2）才停。
                // 只看「凑够两对」会停在满血假块上（实测踩过）。
                bool selfFound = false; int others = 0;
                for (auto& h : hits) {
                    if (std::fabs(h.hp - g_barL) < 0.08f) selfFound = true;
                    else ++others;
                }
                // 敌方要凑到「两对」才停：只碰到零散单块会选错对象
                if (selfFound && others >= 4) stop = true;
            } else {
                ents = BuildEntities(hits);
                if (ents.size() >= 2) stop = true;   // 都满血 / 无血条：凑齐两对即停
            }
            if (stop) { InterlockedExchange(&g_scanStop, 1); break; }
        }
    }
    for (HANDLE h : ths) { WaitForSingleObject(h, 30000); CloseHandle(h); }
    EnterCriticalSection(&g_scanCs);
    hits = g_scanHits;
    uint64_t bytes = (uint64_t)g_scanBytes;
    LeaveCriticalSection(&g_scanCs);
    ents = BuildEntities(hits);
    g_scanBudget = bytes;
    printf("[计时] 分块 %lld 个, 读取(含传输) %lld ms, 解压 %lld ms, 原始命中 %zu 条, 实体 %zu 个\n",
           (long long)g_nChunk, (long long)g_tRead, (long long)g_tGunzip, hits.size(), ents.size());
    for (size_t i = 0; i < hits.size() && i < 12; ++i)
        printf("    hit %012llX hp=%.4f owner=%012llX\n",
               (unsigned long long)hits[i].va, hits[i].hp, (unsigned long long)hits[i].owner);
    g_lastHits = hits;                   // 诊断保留全量（含上一局残留块）

    // 只留「同一 owner 下至少还有一份」的块：本局的两个实体永远各有 A/B 两份、
    // 共享一个 owner；上一局残留的孤块 owner 各不相同。不筛掉就会拿旧值当本局读数
    // （用户那张截图里「我方 34.9%」就是上一局残留的 0x74D89FE9AEF8）。
    {
        std::map<uint64_t, int> oc;
        for (auto& h : hits) oc[h.owner]++;
        std::vector<HpHit> live;
        for (auto& h : hits) if (oc[h.owner] >= 2) live.push_back(h);
        if (live.size() >= 2) { hits.swap(live); ents = BuildEntities(hits); }
    }

    auto pairOf = [&](const HpHit& hb) -> Entity {
        const HpHit* mate = nullptr;
        uint64_t bestD = ~0ULL;
        for (auto& h : hits) {
            if (h.va == hb.va) continue;
            uint64_t d = h.va > hb.va ? h.va - hb.va : hb.va - h.va;
            if (d < 0x40 || d > 0x8000) continue;
            // 同一实体的另一路：未结算(0) 或 值基本一致
            if (!(h.hp < 0.001f || std::fabs(h.hp - hb.hp) < 0.08f)) continue;
            if (d < bestD) { bestD = d; mate = &h; }
        }
        if (!mate) return Entity{ hb.va, hb.va, hb.owner, 0, hb.hp, hb.hp };
        uint64_t lo = std::min(hb.va, mate->va), hi = std::max(hb.va, mate->va);
        float hlo = (hb.va < mate->va) ? hb.hp : mate->hp;
        float hhi = (hb.va < mate->va) ? mate->hp : hb.hp;
        return Entity{ lo, hi, hb.owner, (uint32_t)(hi - lo), hlo, hhi };
    };

    Entity e0{}, e1{};
    bool matched = false;

    auto inEnt = [](const Entity& e, const HpHit& h) {
        return h.va == e.vaA || h.va == e.vaB || (h.va > e.vaA && h.va < e.vaB);
    };
    auto entVal = [](const Entity& e) { return std::max(e.a, e.b); };  // 结算值常常滞后为 0
    bool barLOk = (g_barL >= 0.02f && g_barL <= 0.985f);
    bool barROk = (g_barR >= 0.02f && g_barR <= 0.985f);

    // ① 我方血条没满 -> 用比例直接锁定我方。这是最可靠的归属依据：
    //    「我方 31% 显示成 100%」就是只按 VA / 求链顺序分配导致的。
    if (barLOk) {
        const HpHit* sh = nullptr; float dS = 1e9f;
        for (auto& h : hits) {
            float d = std::fabs(h.hp - g_barL);
            if (d < dS) { dS = d; sh = &h; }
        }
        if (sh && dS < 0.08f) {
            e0 = pairOf(*sh);
            // 敌方：右上血条实测值最接近的那对（差 <=0.10 才算对上）
            if (barROk) {
                float best = 1e9f; Entity be{}; bool have = false;
                for (auto& h : hits) {
                    if (inEnt(e0, h)) continue;
                    float d = std::fabs(h.hp - g_barR);
                    if (d < best && d <= 0.10f) { best = d; be = pairOf(h); have = true; }
                }
                if (have) { e1 = be; matched = true; }
            }
            // 右上条测不到：优先「已经掉过血」的实体（实证过：假对象恒为 1.0）。
            // 2026-11-09 实测：同 owner 下 4 个满血块里，真正参战那对被打到 77.8%，
            // 另外两对全程 1.0 纹丝不动 —— 绝不能按「最接近满血」挑（以前就是这么画反的）。
            if (!matched) {
                float bestScore = 1e9f;
                for (auto& h : hits) {
                    if (inEnt(e0, h)) continue;
                    Entity cand = pairOf(h);
                    float score = (h.hp < 0.999f) ? 0.0f : 0.5f;
                    if (!cand.delta) score += 0.25f;               // 配不上对的分低
                    if (cand.vaA == e1.vaA) score -= 0.005f;       // 同分时保持原选择稳定
                    if (score < bestScore) { bestScore = score; e1 = cand; matched = true; }
                }
            }
            if (matched)
                printf("[归属] 左条=%.3f 右条=%.3f -> 我方 %012llX / 敌方 %012llX\n",
                       g_barL, g_barR, (unsigned long long)e0.vaA, (unsigned long long)e1.vaA);
        }
    }
    // ② 我方满血 / 血条测不到 -> 先看右上条，再按「谁掉过血」，最后按低 VA
    //    （三次普攻实证：我方血块 VA 更低）
    if (!matched) {
        if (ents.size() < 2)
            return fail(L"等待进入战斗…（血量出现后自动显示）");

        // ②-1 右上血条能量到：敌方 = 最接近它的实体
        if (barROk) {
            float best = 1e9f; int bi = -1;
            for (int i = 0; i < (int)ents.size(); ++i) {
                float d = std::fabs(entVal(ents[i]) - g_barR);
                if (d < best && d <= 0.10f) { best = d; bi = i; }
            }
            if (bi >= 0) {
                e1 = ents[bi];
                bool have = false;
                for (auto& e : ents) {
                    if (e.vaA == e1.vaA) continue;
                    if (!have || e.vaA < e0.vaA) { e0 = e; have = true; }
                }
                matched = (have);
                if (matched)
                    printf("[归属] 我方满血，右条=%.3f -> 敌方 %012llX\n",
                           g_barR, (unsigned long long)e1.vaA);
            }
        }
        // ②-2 只有一个实体掉过血：那个就是对面（唯一实证线索）
        if (!matched) {
            std::vector<Entity> damaged;
            for (auto& e : ents) if (e.a < 0.999f || e.b < 0.999f) damaged.push_back(e);
            if (damaged.size() == 1) {
                e1 = damaged[0];
                bool have = false;
                for (auto& e : ents) {
                    if (e.vaA == e1.vaA) continue;
                    if (!have || e.vaA < e0.vaA) { e0 = e; have = true; }
                }
            } else {
                e0 = ents[0]; e1 = ents[1];
                if (e0.vaA > e1.vaA) std::swap(e0, e1);
            }
        }
    }

    g_cachePid     = pid;
    g_cacheVa[0]   = e0.vaA; g_cacheVaB[0] = e0.vaB; g_cachePair[0] = e0.delta;
    g_cacheVa[1]   = e1.vaA; g_cacheVaB[1] = e1.vaB; g_cachePair[1] = e1.delta;
    SaveCache();

    EnterCriticalSection(&g_cs);
    for (int k = 0; k < 2; ++k) {
        SideState& S = g_sides[k];
        S.directVa  = g_cacheVa[k];
        S.directVaB = g_cacheVaB[k];
        S.pairDelta = g_cachePair[k] ? g_cachePair[k] : 0x900;
        S.ready = false; S.init = false; S.ok = false;
        S.leadA = S.leadB = 0;
    }
    g_locNote = L"已自动定位";
    LeaveCriticalSection(&g_cs);

    // 立刻做一次完整直读校验，失败就回到等待状态
    int verified = 0;
    for (int k = 0; k < 2; ++k) if (ReadSideFull(g_sides[k])) ++verified;
    if (verified == 0) return fail(L"血块已变化，正在重新定位…");

    AutoPickMechIfDead();

    InterlockedExchange(&g_locProgress, 100);
    InterlockedExchange(&g_locState, LOC_OK);
    return true;
}

// 把双方四路读数合成面板候选（调用方持有 g_cs）
// 索引：0=我方A 1=我方B 2=敌方A 3=敌方B；机制 mech(0/1) 选中 我方[mech] 与 敌方[2+mech]
static void RebuildCands() {
    g_cands.clear(); g_candVa.clear();
    const wchar_t* nm[4] = { L"我方A 结算", L"我方B 同步", L"敌方A 结算", L"敌方B 同步" };
    const float    vv[4] = { g_sides[0].a, g_sides[0].b, g_sides[1].a, g_sides[1].b };
    const uint64_t aa[4] = { g_sides[0].vaA, g_sides[0].vaB, g_sides[1].vaA, g_sides[1].vaB };
    const bool     oo[4] = { g_sides[0].ok, g_sides[0].ok, g_sides[1].ok, g_sides[1].ok };
    for (int i = 0; i < 4; ++i) {
        Candidate c{}; c.isRatio = true; c.ok = oo[i];
        c.name = nm[i]; c.value = vv[i]; c.addr = aa[i];
        g_cands.push_back(c); g_candVa.push_back(aa[i]);
    }
    if (g_mech < 0 || g_mech > 1) g_mech = 0;
    g_haveData = g_sides[0].ok || g_sides[1].ok;
}

// 完整扫描：定位并分组所有血量块，挑出"自己"（离线标定参考，默认运行路径不调用）
static bool DoFullScan(std::vector<HpGroup>& groups, float& bar) {
    AdbRoot();
    int pid = 0;
    if (!GetGamePid(pid)) return false;
    g_pid = pid;

    auto regs = GetRegions(pid);
    std::vector<std::pair<float, uint64_t>> hits;
    for (auto& pr : regs) {
        std::vector<BYTE> d;
        if (!PullRegion(pid, pr.first, pr.second - pr.first, d)) continue;
        ScanRegion(d, pr.first, hits);
    }
    if (hits.empty()) return false;

    std::sort(hits.begin(), hits.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
    groups.clear();
    for (auto& h : hits) {
        if (!groups.empty() && std::fabs(groups.back().hp - h.first) < 1e-4)
            groups.back().vas.push_back(h.second);
        else
            groups.push_back({ h.first, { h.second } });
    }

    bar = MeasureBar();

    // 选与血条实测最接近的那一组作为"自己"
    int gi = 0; float best = 1e9f;
    for (int i = 0; i < (int)groups.size(); ++i) {
        float dif = std::fabs(groups[i].hp - bar);
        if (dif < best) { best = dif; gi = i; }
    }
    HpGroup self = groups[gi];
    groups.erase(groups.begin() + gi);
    groups.insert(groups.begin(), self);  // 自己放最前
    return true;
}

// ------------------------------------------------------------------ 绘制

static ULONG_PTR g_gdiToken = 0;

struct Surface {
    HBITMAP bmp = nullptr;
    HDC     dc  = nullptr;
    void*   bits = nullptr;
    int     w = 0, h = 0;

    void Release() {
        if (bmp) { DeleteObject(bmp); bmp = nullptr; }
        if (dc)  { DeleteDC(dc); dc = nullptr; }
        bits = nullptr; w = h = 0;
    }
    void Ensure(int nw, int nh) {
        if (w == nw && h == nh && bmp) return;
        Release();
        w = nw; h = nh;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (bmp) SelectObject(dc, bmp);
    }
};

static void DrawRoundRect(Graphics& g, const RectF& r, float rad, Brush* fill, Pen* border) {
    GraphicsPath p;
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
    if (fill)   g.FillPath(fill, &p);
    if (border) g.DrawPath(border, &p);
}

struct Layout {
    int mainW, mainH;
    int barGap, barRowsH;   // 两条主血条之间的间距与总高
    int diagW, diagH;
    int diagX, diagY;
    int totalW, totalH;
};

static Layout MakeLayout(int clientW, int clientH) {
    Layout L{};
    L.mainW = std::max((int)std::lround(g_view.w * clientW), 70);
    L.mainH = std::max((int)std::lround(g_view.h * clientH), 12);
    L.barGap = 4;
    L.barRowsH = L.mainH * 2 + L.barGap;
    L.diagW = 296;
    L.diagH = 6 * 16 + 8;      // 4 行读数 + 1 行切换提示 + 1 行统计
    L.diagX = L.mainW + 6;
    L.diagY = 0;
    L.totalW = L.mainW + 6 + L.diagW;
    L.totalH = std::max(L.barRowsH, L.diagH);
    return L;
}

// 画一条血条：row0=我方(红橙，从左填充)  row1=敌方(蓝紫，从右镜像填充)
static void DrawBarRow(Graphics& g, const Layout& L, int row,
                       const Candidate& s) {
    bool enemy = (row == 1);
    float y = (float)(row * (L.mainH + L.barGap));
    RectF rc(0.5f, y + 0.5f, (REAL)L.mainW - 1.0f, (REAL)L.mainH - 1.0f);
    SolidBrush bg(Color(190, 18, 18, 18));
    Pen border(Color(230, 245, 245, 245), 1.4f);
    DrawRoundRect(g, rc, 3.0f, &bg, &border);

    // 断链时保留上一次有效读数（灰化显示），不再清零 —— 清零再弹回就是「蹦迪」。
    float frac = std::clamp(s.value, 0.0f, 1.0f);
    if (frac > 0.001f) {
        float full = (REAL)L.mainW - 4.0f;
        float fw = full * frac;
        float fx = enemy ? ((REAL)L.mainW - 2.0f - fw) : 2.0f;
        RectF fillRc(fx, y + 2.0f, fw, (REAL)L.mainH - 4.0f);
        GraphicsPath clip;
        {
            float rad = 2.0f, d = rad * 2;
            RectF r(2.0f, y + 2.0f, full, (REAL)L.mainH - 4.0f);
            clip.AddArc(r.X, r.Y, d, d, 180, 90);
            clip.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
            clip.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
            clip.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
            clip.CloseFigure();
        }
        g.SetClip(&clip);
        Color c1 = enemy ? Color(255, 120, 190, 255) : Color(255, 255, 96, 64);
        Color c2 = enemy ? Color(255, 40, 80, 220)   : Color(255, 190, 24, 24);
        if (!s.ok) { c1 = Color(255, 150, 150, 150); c2 = Color(255, 90, 90, 90); }
        LinearGradientBrush lg(PointF(0, y), PointF(0, y + (REAL)L.mainH), c1, c2);
        g.FillRectangle(&lg, fillRc);
        g.ResetClip();
    }

    wchar_t txt[64];
    swprintf_s(txt, L"%ls %.1f%%", enemy ? L"敌方" : L"我方", s.value * 100.f);

    Font font(L"Microsoft YaHei UI", (REAL)std::max(10, L.mainH - 8), FontStyleBold, UnitPixel);
    SolidBrush shadow(Color(180, 0, 0, 0));
    SolidBrush white(Color(255, 255, 255, 255));
    StringFormat sf;
    sf.SetAlignment(enemy ? StringAlignmentNear : StringAlignmentFar);
    sf.SetLineAlignment(StringAlignmentCenter);
    RectF tr(5.0f, y, (REAL)L.mainW - 10.0f, (REAL)L.mainH);
    g.DrawString(txt, -1, &font, tr, &sf, &shadow);
    tr.Offset(0, -1.0f);
    g.DrawString(txt, -1, &font, tr, &sf, &white);
}

static void RenderPanel(Graphics& g, const Layout& L,
                        const std::vector<Candidate>& cands, int mech) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    static Candidate s_empty{};
    auto pick = [&](int i) -> const Candidate& {
        return (i >= 0 && i < (int)cands.size()) ? cands[i] : s_empty;
    };

    // ---------- 两条主血条：我方 / 敌方 ----------
    DrawBarRow(g, L, 0, pick(mech));
    if (g_showEnemy) DrawBarRow(g, L, 1, pick(2 + mech));

    // ---------- 诊断面板：我方A/B + 敌方A/B ----------
    {
        float bx0 = (REAL)L.diagX, by0 = (REAL)L.diagY;
        RectF prc(bx0 + 0.5f, by0 + 0.5f, (REAL)L.diagW - 1.0f, (REAL)L.diagH - 1.0f);
        SolidBrush pbg(Color(165, 0, 0, 0));
        Pen pborder(Color(120, 255, 255, 255), 1.0f);
        DrawRoundRect(g, prc, 4.0f, &pbg, &pborder);

        Font fSmall(L"Microsoft YaHei UI", 11.0f, FontStyleRegular, UnitPixel);
        Font fTag(L"Consolas", 10.0f, FontStyleRegular, UnitPixel);
        SolidBrush white(Color(235, 235, 235, 235));
        SolidBrush gray(Color(200, 140, 140, 140));
        SolidBrush hiSelf(Color(255, 255, 214, 64));
        SolidBrush hiEnemy(Color(255, 130, 210, 255));
        SolidBrush barBg(Color(150, 60, 60, 60));

        int n = (int)cands.size();
        for (int i = 0; i < n && i < 4; ++i) {
            const Candidate& c = cands[i];
            float ly = by0 + 4.0f + (float)(i * 16);
            bool isSel = (i == mech || i == 2 + mech);
            SolidBrush& col = isSel ? (i >= 2 ? hiEnemy : hiSelf) : white;

            wchar_t line[96];
            if (c.ok) swprintf_s(line, L"%s %.1f%%", c.name.c_str(), c.value * 100.f);
            else      swprintf_s(line, L"%s  定位中", c.name.c_str());
            g.DrawString(line, -1, &fSmall, PointF(bx0 + 6.0f, ly), &col);

            float bx = bx0 + 132.0f, bw = 74.0f, bh = 6.0f, by = ly + 5.0f;
            g.FillRectangle(&barBg, RectF(bx, by, bw, bh));
            float f2 = c.ok ? std::clamp(c.value, 0.0f, 1.0f) : 0.0f;
            SolidBrush fb(isSel ? (i >= 2 ? Color(255, 96, 176, 255) : Color(255, 255, 96, 64))
                                : Color(200, 160, 160, 160));
            g.FillRectangle(&fb, RectF(bx, by, bw * f2, bh));

            wchar_t addrTxt[24];
            swprintf_s(addrTxt, L"%llX", (unsigned long long)c.addr);
            g.DrawString(addrTxt, -1, &fTag, PointF(bx0 + 214.0f, ly + 1.0f), &gray);
        }

        // ---------- 切换提示 / 先变统计 ----------
        {
            int row = std::min(n, 4);
            SolidBrush tipC(Color(255, 150, 215, 255));
            SolidBrush statC(Color(235, 170, 170, 170));
            wchar_t tip[128], stat[128];
            if (!g_haveData) {
                // 换号 / 重启 / 换角色：程序自己重定位，绝不提示运行任何脚本
                const wchar_t* note = g_locNoteSnap[0] ? g_locNoteSnap : L"正在自动定位…";
                swprintf_s(tip, L"%ls", note);
                swprintf_s(stat, L"全自动接管  [F5]可立即重新定位");
            } else {
                swprintf_s(tip, L"[F1]结算 [F2]同步  当前:%ls  [F3]敌方:%ls",
                           mech == 0 ? L"结算" : L"同步",
                           g_showEnemy ? L"显示" : L"隐藏");
                swprintf_s(stat, L"先变 我%d/%d 敌%d/%d   Δ+0x%X/+0x%X",
                           g_sides[0].leadA, g_sides[0].leadB,
                           g_sides[1].leadA, g_sides[1].leadB,
                           g_sides[0].delta, g_sides[1].delta);
            }
            g.DrawString(tip,  -1, &fSmall, PointF(bx0 + 6.0f, by0 + 4.0f + row * 16.0f), &tipC);
            g.DrawString(stat, -1, &fSmall, PointF(bx0 + 6.0f, by0 + 4.0f + (row + 1) * 16.0f), &statC);
        }
    }
}

// ------------------------------------------------------------------ 窗口定位

struct FindCtx { DWORD pid; HWND best; long bestArea; };

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (ctx->pid && wpid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    long area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > ctx->bestArea && area > 100) {
        ctx->bestArea = area;
        ctx->best = hwnd;
    }
    return TRUE;
}

static HWND FindEmuWindow() {
    // 优先按外壳进程名找；找不到就退而取最大可见窗口（通常是模拟器）
    DWORD wp = 0;
    if (!g_winProcName.empty()) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, g_winProcName.c_str()) == 0) { wp = pe.th32ProcessID; break; }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }
    if (wp) {
        FindCtx ctx{wp, nullptr, 0};
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.best) return ctx.best;
    }
    FindCtx ctx{0, nullptr, 0};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// ------------------------------------------------------------------ 快照

static void Snapshot(std::vector<Candidate>& out, int& sel) {
    EnterCriticalSection(&g_cs);
    out = g_cands;
    sel = g_mech;
    g_locStateSnap    = (int)g_locState;
    g_locProgressSnap = (int)g_locProgress;
    wcsncpy_s(g_locNoteSnap, g_locNote.c_str(), _TRUNCATE);
    LeaveCriticalSection(&g_cs);
}

// ------------------------------------------------------------------ 扫描线程

// ini 只影响显示参数；热重载绝不动已定位的血块，否则会把读数重置成「蹦迪」。
static void ReloadLightConfig() {
    std::wstring ap = IniStr(L"adb", L"path", L"");
    if (!ap.empty()) g_adbPath = ap;
    g_winProcName = IniStr(L"proc", L"window", L"dnplayer.exe");
    g_view.x = IniDouble(L"view", L"x", g_view.x);
    g_view.y = IniDouble(L"view", L"y", g_view.y);
    g_view.w = IniDouble(L"view", L"w", g_view.w);
    g_view.h = IniDouble(L"view", L"h", g_view.h);
}

// 用屏幕血条交叉校验内存读数。0=一致或没法判断  1=左右认反了  2=读数与画面对不上（要重扫）
// 比较用「同步值」(b)：屏幕血条是实时的，结算值本来就会滞后。
static int CrossCheckSides() {
    if (!g_sides[0].ok || !g_sides[1].ok) return 0;
    float bL = -1.f, bR = -1.f;
    if (!ProbeBarsOnce(bL, bR)) return 0;
    if (bL < 0.02f || bR < 0.02f) return 0;         // 血条不可见（菜单 / 切场景）
    float s0 = g_sides[0].b, s1 = g_sides[1].b;
    float dOK  = std::fabs(s0 - bL) + std::fabs(s1 - bR);
    float dSwp = std::fabs(s1 - bL) + std::fabs(s0 - bR);
    if (dSwp < 0.15f && dSwp + 0.05f < dOK) {
        printf("[纠偏] 左右认反：读数 我=%.3f 敌=%.3f  屏幕 左=%.3f 右=%.3f\n", s0, s1, bL, bR);
        return 1;
    }
    if (dOK > 0.25f) {
        printf("[纠偏] 读数对不上屏幕：我=%.3f 敌=%.3f  屏幕 左=%.3f 右=%.3f\n", s0, s1, bL, bR);
        return 2;
    }
    return 0;
}

// 只交换「我方 / 敌方」两方，不重新扫描
static void SwapSides() {
    EnterCriticalSection(&g_cs);
    std::swap(g_sides[0], g_sides[1]);
    for (int k = 0; k < 2; ++k) {
        g_cacheVa[k]   = g_sides[k].vaA;
        g_cacheVaB[k]  = g_sides[k].vaB;
        g_cachePair[k] = g_sides[k].delta ? g_sides[k].delta : 0x900;
    }
    RebuildCands();
    LeaveCriticalSection(&g_cs);
    SaveCache();
    printf("[纠偏] 已对调我方/敌方\n");
}

static DWORD WINAPI ScannerProc(LPVOID) {
    while (!g_exit) {
        if (IniChanged()) ReloadLightConfig();

        // 模拟器重启 / adb 掉线：自动枚举设备、挑出跑着游戏的那个、确保 root
        if (!EnsureAdb(false)) {
            EnterCriticalSection(&g_cs);
            g_locNote = L"等待模拟器连接…（自动重连）";
            for (int k = 0; k < 2; ++k) g_sides[k].ok = false;
            RebuildCands();
            LeaveCriticalSection(&g_cs);
            InterlockedExchange(&g_locState, LOC_WAIT);
            Sleep(1500);
            continue;
        }
        int pid = 0;
        if (!GetGamePid(pid)) {
            EnterCriticalSection(&g_cs);
            g_locNote = L"等待游戏启动…（自动接管）";
            for (int k = 0; k < 2; ++k) g_sides[k].ok = false;
            RebuildCands();
            LeaveCriticalSection(&g_cs);
            InterlockedExchange(&g_locState, LOC_WAIT);
            g_adbReady = false;          // 强制下次重新探测设备 / 重取 root
            InterlockedExchange(&g_needLocate, 1);
            Sleep(1500);
            continue;
        }
        if (pid != g_pid || !g_modBase) {
            g_pid = pid;
            uint64_t b = FindModuleBase(pid);
            if (b) { g_modBase = b; g_basePid = pid; }
            InterlockedExchange(&g_needLocate, 1);   // 进程换了，运行时 VA 必然失效
        }

        if (InterlockedCompareExchange(&g_needLocate, 0, 0)) {
            // ① 先试 address.cache：形状校验过就 <1 秒直接显示，零扫描
            bool quick = false;
            if (g_cacheVa[0] && g_cacheVa[1]) {
                bool q[2] = { false, false };
                for (int k = 0; k < 2; ++k) q[k] = ReadSideFull(g_sides[k]);
                quick = q[0] && q[1];
                if (quick) {
                    AutoPickMechIfDead();
                    EnterCriticalSection(&g_cs);
                    for (int k = 0; k < 2; ++k) NoteSideLead(g_sides[k]);
                    g_locNote = L"已自动定位";
                    RebuildCands();
                    LeaveCriticalSection(&g_cs);
                    InterlockedExchange(&g_locProgress, 100);
                    InterlockedExchange(&g_locState, LOC_OK);
                }
            }
            // ② 缓存失效（换号 / 重启 / 换角色）：exe 自己重新定位，用户零操作
            if (!quick && !AutoLocate()) {
                Sleep(2000);
                continue;              // 保持 needLocate=1，进战斗后自动成功
            }
            InterlockedExchange(&g_needLocate, 0);
        }

        // ③ 稳态：只读已定位的那几页，不重走链、不串台、不蹦迪
        int failCnt = 0;
        g_pairTick = 0;
        int checkTick = 0, swapVotes = 0, badVotes = 0;
        float prev0 = -1.f, prev1 = -1.f;
        while (!g_exit && !InterlockedCompareExchange(&g_needLocate, 0, 0)) {
            if (WaitForSingleObject(g_scanEvent, 250) == WAIT_OBJECT_0) break;
            bool rewalk = (++g_pairTick >= 10);
            if (rewalk) g_pairTick = 0;
            int good = 0;
            bool fresh[2] = { false, false };
            for (int k = 0; k < 2; ++k) {
                bool r = rewalk ? ReadSideFull(g_sides[k]) : ReadSideFast(g_sides[k]);
                if (!r && g_sides[k].ready) r = ReadSideFast(g_sides[k]);
                fresh[k] = r;
                if (r) ++good;
            }
            EnterCriticalSection(&g_cs);
            for (int k = 0; k < 2; ++k) {
                if (fresh[k]) NoteSideLead(g_sides[k]);
                else g_sides[k].ok = false;
            }
            RebuildCands();
            LeaveCriticalSection(&g_cs);
            if (good == 0) {
                if (++failCnt > 2) { InterlockedExchange(&g_needLocate, 1); break; }
            } else failCnt = 0;

            // 每 ~3 秒拿屏幕血条交叉校验一次：内存读数和画面对不上就自己纠偏
            // （换局后缓存指向上一局的旧对象、或左右认反，都在这里被拉回来，用户零操作）
            // 只在读数「稳定」的回合比：血条有掉血拖影动画，打斗中会短暂落后于内存值，
            // 拿它当基准会误判、把好好的定位推翻重扫。
            float cur0 = g_sides[0].b, cur1 = g_sides[1].b;
            bool steady = (prev0 >= 0.f &&
                           std::fabs(cur0 - prev0) < 0.004f && std::fabs(cur1 - prev1) < 0.004f);
            prev0 = cur0; prev1 = cur1;
            if (!steady) { checkTick = 0; swapVotes = 0; badVotes = 0; continue; }

            if (++checkTick >= 12) {
                checkTick = 0;
                int rc2 = CrossCheckSides();
                if (rc2 == 2) {                       // 连续两次都对不上才重扫，防特效遮挡误判
                    swapVotes = 0;
                    if (++badVotes >= 2) {
                        badVotes = 0;
                        InterlockedExchange(&g_needLocate, 1);
                        break;
                    }
                } else if (rc2 == 1) {
                    badVotes = 0;
                    if (++swapVotes >= 2) { swapVotes = 0; SwapSides(); }
                } else {
                    swapVotes = 0; badVotes = 0;
                }
            }
        }
    }
    return 0;
}

// ------------------------------------------------------------------ 诊断 / 预览

// forceFull=false：先按 address.cache 秒验（就是正常启动走的路径）；
// forceFull=true（--locate）：跳过缓存，强制重新定位一次。
static void PrintDump(bool forceFull = false) {
    printf("== 火影忍者 血条叠加层 · 自动定位诊断 ==\n");
    printf("地址缓存: %ls\n", CachePath().c_str());
    bool ok = false;
    if (!forceFull) {
        EnsureAdb(true);
        int pid = 0;
        if (GetGamePid(pid)) {
            g_pid = pid;
            g_modBase = FindModuleBase(pid);
            g_basePid = pid;
        }
        if (g_cacheVa[0] && g_cacheVa[1])
            ok = ReadSideFull(g_sides[0]) && ReadSideFull(g_sides[1]);
        if (ok) {
            AutoPickMechIfDead();
            EnterCriticalSection(&g_cs);
            g_locNote = L"已自动定位";
            RebuildCands();
            LeaveCriticalSection(&g_cs);
            InterlockedExchange(&g_locState, LOC_OK);
        }
    }
    if (!ok) ok = AutoLocate();
    printf("adb  : %ls   serial: %ls\n", g_adbPath.c_str(), g_adbSer.c_str());
    printf("定位 : %s (%s)   PID=%d   libil2cpp.so=%012llX\n",
           ok ? "成功" : "未找到（需进入战斗，程序会自动重试）",
           forceFull ? "全量定位" : (g_scanBudget ? "自动重定位" : "缓存直读"),
           g_pid, (unsigned long long)g_modBase);
    if (g_scanBudget) {
        printf("扫描 : 本次读取 %llu MB（命中即停）\n",
               (unsigned long long)(g_scanBudget / 1048576ULL));
        printf("血条 : 左(我) %.1f%%   右(敌) %.1f%%\n", g_barL * 100.f, g_barR * 100.f);
    }
    const wchar_t* nm[2] = { L"我方", L"敌方" };
    for (int k = 0; k < 2; ++k) {
        SideState& S = g_sides[k];
        printf("\n---- %ls ----\n", nm[k]);
        printf("值A VA = %012llX   值B VA = %012llX   Δ = 0x%X\n",
               (unsigned long long)S.vaA, (unsigned long long)S.vaB, S.delta);
        printf("owner  = %012llX\n", (unsigned long long)S.owner);
        printf("值A 结算 = %.4f (%.1f%%)   值B 同步 = %.4f (%.1f%%)   校验: %s\n",
               S.a, S.a * 100.f, S.b, S.b * 100.f, S.ok ? "PASS" : "FAIL");
    }
    if (!g_lastHits.empty()) {
        std::vector<HpHit> hs = g_lastHits;
        // 非满血（真正在掉血的）排前面，诊断时一眼能看到
        std::stable_sort(hs.begin(), hs.end(), [](const HpHit& a, const HpHit& b) {
            bool fa = (a.hp > 0.001f && a.hp < 0.999f), fb = (b.hp > 0.001f && b.hp < 0.999f);
            if (fa != fb) return fa;
            return a.hp < b.hp;
        });
        printf("\n全部血量块 (%zu)  [非满血优先]:\n", hs.size());
        size_t shown = 0;
        for (auto& h : hs) {
            if (shown++ >= 60) { printf("  ... 其余 %zu 个省略\n", hs.size() - 60); break; }
            printf("  %012llX  %.4f  owner=%012llX%s\n",
                   (unsigned long long)h.va, h.hp, (unsigned long long)h.owner,
                   (h.hp > 0.001f && h.hp < 0.999f) ? "  <= 非满血" : "");
        }
    }
    printf("\n机制: %ls   敌方条: %ls\n", g_mech == 0 ? L"值A(结算)" : L"值B(同步)",
           g_showEnemy ? L"显示" : L"隐藏");
}

static int SaveEncoderClsid(const WCHAR* mime, CLSID* clsid);

static void DoPreview(const wchar_t* outPath, bool fake = false, bool fakeB = false) {
    if (fake) {
        // 演示：双方两路读数都不同（模拟扣血瞬间），fakeB 时用值B 机制
        EnterCriticalSection(&g_cs);
        g_sides[0].ok = g_sides[1].ok = true;
        g_sides[0].a = 0.473f; g_sides[0].b = 0.730f;
        g_sides[1].a = 0.918f; g_sides[1].b = 0.960f;
        g_sides[0].vaA = 0x7F2AE59A43B8ULL; g_sides[0].vaB = 0x7F2AE59A4CB8ULL;
        g_sides[1].vaA = 0x7F2AE5BEC5F8ULL; g_sides[1].vaB = 0x7F2AE5BECEF8ULL;
        g_sides[0].delta = g_sides[1].delta = 0x900;
        g_sides[0].leadA = 3; g_sides[0].leadB = 7;
        g_sides[1].leadA = 1; g_sides[1].leadB = 5;
        RebuildCands();
        g_mech = fakeB ? 1 : 0;
        LeaveCriticalSection(&g_cs);
    } else {
        if (AutoLocate()) {
            EnterCriticalSection(&g_cs);
            RebuildCands();
            LeaveCriticalSection(&g_cs);
        }
    }

    int cw = 1088, ch = 613;
    HWND hwnd = FindEmuWindow();
    if (hwnd) {
        RECT cr{}; GetClientRect(hwnd, &cr);
        cw = cr.right - cr.left; ch = cr.bottom - cr.top;
    }
    Layout L = MakeLayout(cw, ch);

    std::vector<Candidate> snap; int mech;
    Snapshot(snap, mech);

    Bitmap bmp(L.totalW, L.totalH, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        RenderPanel(g, L, snap, mech);
    }
    CLSID png{};
    if (SaveEncoderClsid(L"image/png", &png) == 0) {
        bmp.Save(outPath, &png, nullptr);
        printf("saved %ls  (%dx%d)\n", outPath, L.totalW, L.totalH);
    } else {
        printf("找不到 PNG 编码器\n");
    }
}

// 把模拟器客户区截成 PNG（用于测量坐标）
// 诊断：放宽特征，枚举所有「像血量」的块（验证敌方块是否尾部特征不同）
static void DoRelax() {
    if (!EnsureAdb(true)) { printf("adb 不可用\n"); return; }
    int pid = 0;
    if (!GetGamePid(pid)) { printf("找不到游戏进程\n"); return; }
    g_pid = pid;
    g_modBase = FindModuleBase(pid);

    auto regs = GetRegions(pid);
    if (regs.empty()) { printf("读取内存映射失败\n"); return; }
    printf("区域 %zu 个，开始放宽扫描（float+8零+1+owner，不校验 TAIL）…\n", regs.size());

    DWORD deadline = GetTickCount() + 180000;
    std::vector<HpHit> hits;
    uint64_t bytes = 0;
    for (auto& r : regs) {
        ScanAreaRelax(r.first, r.second, hits, bytes, deadline);
        if (GetTickCount() >= deadline) break;
    }
    printf("读取 %llu MB，命中 %zu 块（其中满血 %zu）\n",
           (unsigned long long)(bytes / 1048576ULL), hits.size(),
           (size_t)std::count_if(hits.begin(), hits.end(),
                                 [](const HpHit& h) { return h.hp >= 0.999f; }));

    int bucket[20] = { 0 };
    for (auto& h : hits) {
        int b = (int)(h.hp * 20.f);
        if (b < 0) b = 0; if (b > 19) b = 19;
        ++bucket[b];
    }
    printf("分布(每 5%%): ");
    for (int i = 0; i < 20; ++i) printf("%d%s", bucket[i], i == 19 ? "\n" : "|");

    std::stable_sort(hits.begin(), hits.end(), [](const HpHit& a, const HpHit& b) {
        return a.hp < b.hp;
    });
    printf("\n所有非 0/1 的候选（最多 120 条）:\n");
    int shown = 0;
    for (auto& h : hits) {
        if (h.hp <= 0.001f || h.hp >= 0.999f) continue;
        printf("  %012llX  %.4f  owner=%012llX\n",
               (unsigned long long)h.va, h.hp, (unsigned long long)h.owner);
        if (++shown >= 120) { printf("  ...省略\n"); break; }
    }
    if (!shown) printf("  （一个都没有）\n");
}

// 诊断：实时监听所有血量块，打印谁在变（用来实证敌方掉血对应哪一块）
static void DoWatch(int seconds) {
    if (!EnsureAdb(true)) { printf("adb 不可用\n"); return; }
    int pid = 0;
    if (!GetGamePid(pid)) { printf("找不到游戏进程\n"); return; }
    g_pid = pid;
    g_modBase = FindModuleBase(pid);

    printf("初次定位…\n");
    if (!AutoLocate()) { printf("定位失败（需在战斗中）\n"); return; }

    std::vector<HpHit> list = g_lastHits;
    // 把缓存里的两路也并进来，避免遗漏
    for (int k = 0; k < 2; ++k) {
        for (uint64_t v : { g_cacheVa[k], g_cacheVaB[k] }) {
            if (!v) continue;
            bool dup = false;
            for (auto& h : list) if (h.va == v) { dup = true; break; }
            if (dup) continue;
            float f = 0.f;
            if (FastReadHp(v, f)) list.push_back({ v, 0, f });
        }
    }
    if (list.empty()) { printf("没有血量块\n"); return; }

    printf("监听 %d 秒，共 %zu 块。现在去打对面！\n", seconds, list.size());
    for (auto& h : list)
        printf("  %012llX  %.4f\n", (unsigned long long)h.va, h.hp);
    fflush(stdout);

    DWORD t0 = GetTickCount();
    int cycles = 0;
    while ((int)((GetTickCount() - t0) / 1000) < seconds && !g_exit) {
        ++cycles;
        // 按页聚合，一页只读一次
        std::vector<std::pair<uint64_t, std::vector<BYTE>>> pages;
        for (auto& h : list) {
            uint64_t pg = h.va & ~0xFFFULL;
            bool have = false;
            for (auto& p : pages) if (p.first == pg) { have = true; break; }
            if (have) continue;
            std::vector<BYTE> d;
            if (ReadGuestPage(pg, d)) pages.push_back({ pg, std::move(d) });
            else pages.push_back({ pg, {} });
        }
        for (auto& h : list) {
            uint64_t pg = h.va & ~0xFFFULL;
            const std::vector<BYTE>* d = nullptr;
            for (auto& p : pages) if (p.first == pg && !p.second.empty()) { d = &p.second; break; }
            if (!d) continue;
            uint32_t off = (uint32_t)(h.va & 0xFFF);
            if (off + 4 > d->size()) continue;
            float v = 0.f; memcpy(&v, d->data() + off, 4);
            if (!std::isfinite(v)) continue;
            if (std::fabs(v - h.hp) > 0.0005f) {
                printf("[%4lus] %012llX  %.4f -> %.4f  %s\n",
                       (unsigned long)((GetTickCount() - t0) / 1000),
                       (unsigned long long)h.va, h.hp, v,
                       v < h.hp ? "DOWN <= 掉血" : "UP");
                fflush(stdout);
                h.hp = v;
            }
        }
        Sleep(300);
    }
    printf("监听结束（%d 轮）\n", cycles);
}

// 裁剪放大某一区域，便于肉眼核对血条
static void DoCrop(const wchar_t* src, const wchar_t* dst,
                   float fx0, float fy0, float fx1, float fy1, int scale) {
    Bitmap* bmp = Bitmap::FromFile(src);
    if (!bmp || bmp->GetLastStatus() != Ok) { printf("加载失败\n"); return; }
    UINT W = bmp->GetWidth(), H = bmp->GetHeight();
    int x0 = (int)(W * fx0), y0 = (int)(H * fy0);
    int w = (int)(W * (fx1 - fx0)), h = (int)(H * (fy1 - fy0));
    if (w <= 0 || h <= 0) { delete bmp; printf("区域非法\n"); return; }
    Bitmap out(w * scale, h * scale, PixelFormat32bppARGB);
    {
        Graphics g(&out);
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(bmp, Rect(0, 0, w * scale, h * scale), x0, y0, w, h, UnitPixel);
    }
    CLSID png{};
    if (SaveEncoderClsid(L"image/png", &png) == 0) {
        out.Save(dst, &png, nullptr);
        printf("saved %ls  %dx%d\n", dst, w * scale, h * scale);
    }
    delete bmp;
}

static void DoShot(const wchar_t* outPath) {    HWND emu = FindEmuWindow();
    if (!emu) { printf("!! 没找到模拟器窗口\n"); return; }
    ShowWindow(emu, SW_RESTORE);
    SetWindowPos(emu, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(emu);
    Sleep(800);

    RECT cr{}; GetClientRect(emu, &cr);
    POINT org{0, 0}; ClientToScreen(emu, &org);
    int w = cr.right - cr.left, h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) { printf("!! 客户区为空\n"); return; }
    printf("客户区 %dx%d  屏幕原点 (%ld,%ld)\n", w, h, org.x, org.y);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP hb = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, hb);
    BitBlt(mem, 0, 0, w, h, screen, org.x, org.y, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    {
        Bitmap bmp(hb, nullptr);
        CLSID png{};
        if (SaveEncoderClsid(L"image/png", &png) == 0)
            bmp.Save(outPath, &png, nullptr);
    }
    DeleteObject(hb);
    printf("saved %ls  %dx%d\n", outPath, w, h);
}

static int SaveEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (size == 0) return -1;
    auto* info = (ImageCodecInfo*)malloc(size);
    if (!info) return -1;
    GetImageEncoders(n, size, info);
    int found = -1;
    for (UINT i = 0; i < n; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; found = 0; break; }
    }
    free(info);
    return found;
}

// ------------------------------------------------------------------ 热键

static void PollHotkeys(bool& running, HWND hwnd) {
    static bool prev[256] = {false};
    auto pressed = [&](int vk) {
        bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool down = now && !prev[vk & 0xFF];
        prev[vk & 0xFF] = now;
        return down;
    };
    // F1/F2 全局机制：值A(结算) / 值B(扣血同步)，我方与敌方同时切换
    if (pressed(VK_F1)) {
        EnterCriticalSection(&g_cs); g_mech = 0; LeaveCriticalSection(&g_cs);
    }
    if (pressed(VK_F2)) {
        EnterCriticalSection(&g_cs); g_mech = 1; LeaveCriticalSection(&g_cs);
    }
    if (pressed(VK_F3)) g_showEnemy = !g_showEnemy;          // 显示 / 隐藏敌方条
    if (pressed(VK_F4)) {
        EnterCriticalSection(&g_cs); g_mech = 0; LeaveCriticalSection(&g_cs);
        g_showEnemy = true;                                   // 复位
    }
    if (pressed(VK_F5)) {                       // 强制立即重新定位
        InterlockedExchange(&g_needLocate, 1);
        if (g_scanEvent) SetEvent(g_scanEvent);
    }
    if (pressed(VK_F8)) g_visible = !g_visible;
    if (pressed(VK_F9)) running = false;
    if (pressed(VK_F7)) { g_nudgeDX = 0; g_nudgeDY = 0; }
    if (pressed(VK_F12)) {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, (g_exeDir + L"\\overlay.png").c_str());
        DoPreview(path);
    }
    if (pressed(VK_F6)) g_nudgeMode = !g_nudgeMode;
    if (g_nudgeMode) {
        if (pressed(VK_LEFT))  g_nudgeDX -= 1;
        if (pressed(VK_RIGHT)) g_nudgeDX += 1;
        if (pressed(VK_UP))    g_nudgeDY -= 1;
        if (pressed(VK_DOWN))  g_nudgeDY += 1;
    }
    (void)hwnd;
}

// ------------------------------------------------------------------ 主循环

static int RunOverlay() {
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"fanyin_overlay",
        WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        printf("CreateWindowEx 失败: %lu\n", GetLastError());
        return 1;
    }

    Surface surf;
    HWND emu = nullptr;
    ULONGLONG lastWinCheck = 0;

    bool running = true;
    MSG msg;
    int lastW = -1, lastH = -1;

    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        PollHotkeys(running, hwnd);

        ULONGLONG now = GetTickCount64();
        if (now - lastWinCheck > 500) {
            lastWinCheck = now;
            emu = FindEmuWindow();
        }
        if (!g_visible || !emu) {
            ShowWindow(hwnd, SW_HIDE);
            Sleep(120);
            continue;
        }

        RECT cr{}, wr{};
        GetClientRect(emu, &cr);
        POINT org{0, 0};
        ClientToScreen(emu, &org);
        int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        if (cw <= 0 || ch <= 0) { Sleep(120); continue; }

        Layout L = MakeLayout(cw, ch);
        int ax = org.x + g_nudgeDX + (int)std::lround(g_view.x * cw);
        int ay = org.y + g_nudgeDY + (int)std::lround(g_view.y * ch);

        std::vector<Candidate> snap; int mech;
        Snapshot(snap, mech);

        surf.Ensure(L.totalW, L.totalH);
        if (!surf.dc) { Sleep(120); continue; }

        {
            Bitmap bmpS(L.totalW, L.totalH, PixelFormat32bppPARGB);
            {
                Graphics g(&bmpS);
                RenderPanel(g, L, snap, mech);
            }
            Rect rcAll(0, 0, L.totalW, L.totalH);
            BitmapData bd{};
            if (bmpS.LockBits(&rcAll, ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Ok) {
                BYTE* dst = static_cast<BYTE*>(surf.bits);
                BYTE* src = static_cast<BYTE*>(bd.Scan0);
                for (int y = 0; y < L.totalH; ++y)
                    memcpy(dst + y * (size_t)L.totalW * 4, src + (size_t)y * bd.Stride,
                           (size_t)L.totalW * 4);
                bmpS.UnlockBits(&bd);
            }
        }

        HDC screen = GetDC(nullptr);
        POINT ptSrc{0, 0};
        POINT ptDst{ax, ay};
        SIZE sz{L.totalW, L.totalH};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        BOOL ulwOk = UpdateLayeredWindow(hwnd, screen, &ptDst, &sz, surf.dc, &ptSrc,
                                         0, &bf, ULW_ALPHA);
        if (!ulwOk) {
            static bool once = false;
            if (!once) { once = true; printf("UpdateLayeredWindow 失败: %lu\n", GetLastError()); }
        }
        ReleaseDC(nullptr, screen);

        if (lastW != L.totalW || lastH != L.totalH || !IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastW = L.totalW; lastH = L.totalH;
        }

        Sleep(30);
    }

    surf.Release();
    if (hwnd) DestroyWindow(hwnd);
    return 0;
}

// ------------------------------------------------------------------ 入口

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, ".UTF-8");

    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using PFN = BOOL(WINAPI*)(HANDLE);
        if (auto p = (PFN)GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
            p((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    InitConfig();
    LoadCache();       // 上次成功定位的血块 VA：命中就秒显示
    ApplyCache();
    IniChanged();      // 建立 ini 时间戳基线

    InitializeCriticalSection(&g_cs);  // 所有模式共用（--dump/--preview 也会用到）
    ScanQueueInit();

    bool isDump   = (argc >= 2 && _wcsicmp(argv[1], L"--dump") == 0);
    bool isDeep   = (argc >= 2 && _wcsicmp(argv[1], L"--deep") == 0);
    bool isWatch  = (argc >= 2 && _wcsicmp(argv[1], L"--watch") == 0);
    bool isCrop   = (argc >= 2 && _wcsicmp(argv[1], L"--crop") == 0);
    bool isRelax  = (argc >= 2 && _wcsicmp(argv[1], L"--relax") == 0);
    bool isLocate = (argc >= 2 && _wcsicmp(argv[1], L"--locate") == 0);
    bool isShot   = (argc >= 2 && _wcsicmp(argv[1], L"--shot") == 0);
    bool isBar    = (argc >= 2 && _wcsicmp(argv[1], L"--bar") == 0);
    bool isBarScr = (argc >= 2 && _wcsicmp(argv[1], L"--barscreen") == 0);
    bool isPrev   = (argc >= 2 && _wcsicmp(argv[1], L"--preview") == 0);
    bool isReadTest = (argc >= 2 && _wcsicmp(argv[1], L"--readtest") == 0);
    bool isPtrScan = (argc >= 2 && _wcsicmp(argv[1], L"--ptrscan") == 0);
    bool isWidgets = (argc >= 2 && _wcsicmp(argv[1], L"--widgets") == 0);

    if (isReadTest) {                    // 自检：读一块内存，报告 gzip/解压是否正常
        AdbRoot();
        int pid = 0;
        if (!GetGamePid(pid)) { printf("找不到游戏进程\n"); return 1; }
        g_pid = pid;
        uint64_t va = (argc >= 3) ? _wcstoui64(argv[2], nullptr, 16) : 0x72E4E0800000ULL;
        uint32_t pages = (argc >= 4) ? (uint32_t)_wtoi(argv[3]) : 2048;
        std::vector<BYTE> d;
        DWORD t0 = GetTickCount();
        bool ok = ReadChunk(va & ~0xFFFULL, pages, d);
        printf("读 %012llX %u页 -> %s, %zu 字节, %lu ms\n",
               (unsigned long long)va, pages, ok ? "成功" : "失败",
               d.size(), (unsigned long)(GetTickCount() - t0));
        if (!g_lastAdbErr.empty()) printf("adb stderr: %s\n", g_lastAdbErr.c_str());
        if (!ok) {
            printf("原始返回(%zu): ", d.size());
            for (BYTE c : d) putchar((c >= 32 && c < 127) ? c : '.');
            printf("\n");
        }
        const BYTE TAILv[24] = { 1,0,0,0, 1,0,0,0, 2,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0 };
        int cnt = 0;
        for (size_t i = 0; i + 24 <= d.size(); ++i)
            if (memcmp(d.data() + i, TAILv, 24) == 0) ++cnt;
        printf("TAIL 特征命中 %d 次\n", cnt);
        return 0;
    }

    if (isWidgets) {                     // 列出全堆所有进度条 widget
        DoWidgets();
        return 0;
    }

    if (isPtrScan) {                     // 指针回溯：--ptrscan <hex> [span]
        uint64_t c = (argc >= 3) ? _wcstoui64(argv[2], nullptr, 16) : 0;
        uint64_t span = (argc >= 4) ? _wcstoui64(argv[3], nullptr, 16) : 0x400ULL;
        DoPtrScan(c - span, c + span);
        return 0;
    }

    GdiplusStartupInput gsi;    if (GdiplusStartup(&g_gdiToken, &gsi, nullptr) != Ok) {
        printf("GDI+ 初始化失败\n");
        return 1;
    }

    int rc = 0;
    if (isDump) {
        PrintDump();
    } else if (isDeep) {
        g_deepScan = true;
        PrintDump(true);
    } else if (isWatch) {
        DoWatch(argc >= 3 ? _wtoi(argv[2]) : 60);
    } else if (isRelax) {
        DoRelax();
    } else if (isCrop) {
        // --crop src out fx0 fy0 fx1 fy1 [scale]
        if (argc >= 8) DoCrop(argv[2], argv[3], (float)_wtof(argv[4]), (float)_wtof(argv[5]),
                              (float)_wtof(argv[6]), (float)_wtof(argv[7]),
                              argc >= 9 ? _wtoi(argv[8]) : 3);
        else printf("用法: --crop src out fx0 fy0 fx1 fy1 [scale]\n");
    } else if (isLocate) {
        PrintDump(true);
    } else if (isShot) {
        DoShot(argc >= 3 ? argv[2] : L"shot.png");
    } else if (isBar) {
        DebugBar(argc >= 3 ? argv[2] : L"game_now.png");
    } else if (isBarScr) {
        DebugBarScreen();
    } else if (isPrev) {
        bool fake = (argc >= 4 && _wcsicmp(argv[3], L"fake") == 0);
        bool fakeB = fake && (argc >= 5 && _wcsicmp(argv[4], L"b") == 0);
        DoPreview(argc >= 3 ? argv[2] : L"preview.png", fake, fakeB);
    } else {
        printf("== 火影忍者 血条叠加层 · 打开即自动定位 ==\n");
        printf("换号 / 换角色 / 重启游戏 / 重启模拟器：全部自动接管，无需运行任何脚本。\n");
        printf("血量块只在战斗中分配，未进战斗时面板显示「等待进入战斗」。\n");
        printf("F1 结算 / F2 同步(双方一起切) / F3 显隐敌方条 / F4 复位 / F5 立即重新定位\n");
        printf("F6 位置微调 / F7 复位 / F8 显隐 / F12 存图 / F9 退出\n");
        printf("地址缓存: %ls\n", CachePath().c_str());

        g_scanEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        HANDLE hThread = CreateThread(nullptr, 0, ScannerProc, nullptr, 0, nullptr);
        rc = RunOverlay();
        g_exit = true;
        if (g_scanEvent) SetEvent(g_scanEvent);
        if (hThread) { WaitForSingleObject(hThread, 60000); CloseHandle(hThread); }
        if (g_scanEvent) CloseHandle(g_scanEvent);
        DeleteCriticalSection(&g_cs);
    }

    GdiplusShutdown(g_gdiToken);
    return rc;
}
