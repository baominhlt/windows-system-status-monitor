// Background sampler. Every source fails independently (-> N/A) and is retried after a few seconds.
#include "sample.h"

#include <dxgi.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <string>
#include <vector>

static const double GB = 1024.0 * 1024 * 1024, MB = 1024.0 * 1024;
static const DWORD RETRY_MS = 5000;

static CRITICAL_SECTION g_cs;
static Snap g_snap;
static HANDLE g_stop, g_thread;
static SamplerCfg* g_cfg;
static HWND g_notify;
static UINT g_msg;

// ---- PDH ----------------------------------------------------------------------------------------
struct PdhSrc {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER eng = nullptr, shared = nullptr, ded = nullptr, idle = nullptr, rd = nullptr, wr = nullptr, therm = nullptr;
};

static PDH_HCOUNTER AddCounter(PDH_HQUERY q, const wchar_t* path) {
    PDH_HCOUNTER h = nullptr;
    return PdhAddEnglishCounterW(q, path, 0, &h) == ERROR_SUCCESS ? h : nullptr;  // missing counter -> that metric is N/A
}

static bool PdhOpen(PdhSrc& p) {
    if (PdhOpenQueryW(nullptr, 0, &p.q) != ERROR_SUCCESS) return false;
    p.eng = AddCounter(p.q, L"\\GPU Engine(*)\\Utilization Percentage");
    p.shared = AddCounter(p.q, L"\\GPU Adapter Memory(*)\\Shared Usage");
    p.ded = AddCounter(p.q, L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    p.idle = AddCounter(p.q, L"\\PhysicalDisk(_Total)\\% Idle Time");
    p.rd = AddCounter(p.q, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec");
    p.wr = AddCounter(p.q, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec");
    p.therm = AddCounter(p.q, L"\\Thermal Zone Information(*)\\Temperature");  // Kelvin, no admin needed
    PdhCollectQueryData(p.q);
    return true;
}

typedef std::vector<std::pair<std::wstring, double>> PdhArray;

static PdhArray ReadArray(PDH_HCOUNTER h) {
    PdhArray out;
    DWORD size = 0, count = 0;
    // The status is a signed LONG: compare as DWORD or the MORE_DATA test never matches.
    if (!h || (DWORD)PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &size, &count, nullptr) != (DWORD)PDH_MORE_DATA) return out;
    std::vector<BYTE> buf(size);
    auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &size, &count, it) != ERROR_SUCCESS) return out;
    for (DWORD i = 0; i < count; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        std::wstring n = it[i].szName;
        std::transform(n.begin(), n.end(), n.begin(), ::towlower);
        out.push_back({ n, it[i].FmtValue.doubleValue });
    }
    return out;
}

static double ReadOne(PDH_HCOUNTER h) {
    PDH_FMT_COUNTERVALUE v;
    if (!h || PdhGetFormattedCounterValue(h, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS) return NA;
    return v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA ? v.doubleValue : NA;
}

// ---- DXGI adapters (order = Windows GPU number) -------------------------------------------------
struct Adapter { wchar_t name[64]; UINT vendor; wchar_t luid[40]; double dedicatedGB, sharedGB; };

static std::vector<Adapter> ListAdapters() {
    std::vector<Adapter> out;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return out;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        a->Release();
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;  // software / Basic Render
        Adapter ad{};
        wcsncpy_s(ad.name, d.Description, _TRUNCATE);
        ad.vendor = d.VendorId;
        swprintf_s(ad.luid, L"luid_0x%08x_0x%08x", (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart);
        ad.dedicatedGB = d.DedicatedVideoMemory / GB;
        ad.sharedGB = d.SharedSystemMemory / GB;
        out.push_back(ad);
    }
    f->Release();
    return out;
}

// ---- NVML (loaded dynamically; absent DLL -> N/A) ------------------------------------------------
struct Nvml {
    HMODULE dll = nullptr;
    void* dev = nullptr;
    int (*init)() = nullptr;
    int (*byIndex)(unsigned, void**) = nullptr;
    int (*util)(void*, unsigned*) = nullptr;   // struct { unsigned gpu, memory; }
    int (*mem)(void*, unsigned long long*) = nullptr;  // struct { total, free, used; }
    int (*shutdown)() = nullptr;
    int (*temp)(void*, int, unsigned*) = nullptr;      // optional: missing -> temperature N/A only
};

static bool NvmlOpen(Nvml& n) {
    n.dll = LoadLibraryW(L"nvml.dll");
    if (!n.dll) {
        wchar_t p[MAX_PATH], pf[MAX_PATH];
        if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
            swprintf_s(p, L"%s\\NVIDIA Corporation\\NVSMI\\nvml.dll", pf);
            n.dll = LoadLibraryW(p);
        }
    }
    if (!n.dll) return false;
    n.init = (int (*)())GetProcAddress(n.dll, "nvmlInit_v2");
    n.byIndex = (int (*)(unsigned, void**))GetProcAddress(n.dll, "nvmlDeviceGetHandleByIndex_v2");
    n.util = (int (*)(void*, unsigned*))GetProcAddress(n.dll, "nvmlDeviceGetUtilizationRates");
    n.mem = (int (*)(void*, unsigned long long*))GetProcAddress(n.dll, "nvmlDeviceGetMemoryInfo");
    n.shutdown = (int (*)())GetProcAddress(n.dll, "nvmlShutdown");
    n.temp = (int (*)(void*, int, unsigned*))GetProcAddress(n.dll, "nvmlDeviceGetTemperature");
    if (!n.init || !n.byIndex || !n.util || !n.mem || n.init() != 0 || n.byIndex(0, &n.dev) != 0) {
        if (n.dll) FreeLibrary(n.dll);
        n = Nvml();
        return false;
    }
    return true;
}

static void NvmlClose(Nvml& n) {
    if (n.shutdown) n.shutdown();
    if (n.dll) FreeLibrary(n.dll);
    n = Nvml();
}

// ---- network -------------------------------------------------------------------------------------
struct NicPrev { ULONG64 luid, in, out; };

static bool VirtualNic(const wchar_t* desc) {
    std::wstring d = desc;
    std::transform(d.begin(), d.end(), d.begin(), ::towlower);
    for (const wchar_t* k : { L"virtual", L"vethernet", L"hyper-v", L"vmware", L"wsl", L"docker", L"bluetooth", L"loopback",
                              L"tap-", L"pseudo", L"teredo", L"isatap", L"npcap", L"tailscale", L"zerotier" })
        if (d.find(k) != std::wstring::npos) return true;
    return false;
}

// ---- thread --------------------------------------------------------------------------------------
static DWORD WINAPI Run(void*) {
    PdhSrc pdh; bool pdhOk = false;
    Nvml nv; bool nvOk = false;
    std::vector<Adapter> ads;
    DWORD pdhRetry = 0, nvRetry = 0, adRetry = 0;
    ULONGLONG pIdle = 0, pKern = 0, pUser = 0; bool haveCpu = false;
    std::vector<NicPrev> prev; ULONGLONG prevT = 0;

    do {
        Snap s;
        DWORD now = GetTickCount();

        // CPU / RAM
        FILETIME fi, fk, fu;
        if (GetSystemTimes(&fi, &fk, &fu)) {
            auto u64 = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
            ULONGLONG i = u64(fi), k = u64(fk), u = u64(fu);
            if (haveCpu && k + u > pKern + pUser)
                s.cpu = 100.0 * (1.0 - (double)(i - pIdle) / (double)((k + u) - (pKern + pUser)));
            if (s.cpu != NA) s.cpu = std::min(100.0, std::max(0.0, s.cpu));
            pIdle = i; pKern = k; pUser = u; haveCpu = true;
        }
        MEMORYSTATUSEX m{ sizeof(m) };
        if (GlobalMemoryStatusEx(&m)) {
            s.ramPct = m.dwMemoryLoad;
            s.ramTotGB = m.ullTotalPhys / GB;
            s.ramUsedGB = (m.ullTotalPhys - m.ullAvailPhys) / GB;
        }

        // PDH (GPU engines/memory + disk)
        if (!pdhOk && (int)(now - pdhRetry) >= 0) { pdhOk = PdhOpen(pdh); if (!pdhOk) pdhRetry = now + RETRY_MS; }
        PdhArray eng, shared, ded;
        if (pdhOk) {
            if (PdhCollectQueryData(pdh.q) != ERROR_SUCCESS) { PdhCloseQuery(pdh.q); pdh = PdhSrc(); pdhOk = false; pdhRetry = now + RETRY_MS; }
            else {
                eng = ReadArray(pdh.eng); shared = ReadArray(pdh.shared); ded = ReadArray(pdh.ded);
                for (auto& z : ReadArray(pdh.therm)) {  // Kelvin -> C; hottest zone, plausible values only
                    double c = z.second - 273.15;
                    if (c > 0 && c < 150) s.sysTempC = std::max(s.sysTempC, c);
                }
                double idle = ReadOne(pdh.idle), rd = ReadOne(pdh.rd), wr = ReadOne(pdh.wr);
                if (idle != NA) s.diskActive = std::min(100.0, std::max(0.0, 100.0 - idle));
                if (rd != NA) s.diskRead = rd / 1e6;
                if (wr != NA) s.diskWrite = wr / 1e6;
            }
        }

        // GPUs
        if (ads.empty() && (int)(now - adRetry) >= 0) { ads = ListAdapters(); if (ads.empty()) adRetry = now + RETRY_MS; }
        bool poll = InterlockedCompareExchange(&g_cfg->nvidia, 0, 0) != 0;
        if (!poll && nvOk) { NvmlClose(nv); nvOk = false; }
        if (poll && !nvOk && (int)(now - nvRetry) >= 0) { nvOk = NvmlOpen(nv); if (!nvOk) nvRetry = now + RETRY_MS; }
        for (size_t i = 0; i < ads.size() && i < 2; i++) {
            const Adapter& a = ads[i];
            GpuSnap& g = s.gpu[i];
            g.present = true; g.n = (int)i;
            wcscpy_s(g.name, a.name);
            if (a.vendor == 0x10DE) {
                g.nvidia = true;
                if (!poll) { g.off = true; continue; }
                unsigned u[2], t = 0; unsigned long long mi[3];
                if (nvOk && nv.util(nv.dev, u) == 0 && nv.mem(nv.dev, mi) == 0) {
                    g.pct = u[0]; g.memTotGB = mi[0] / GB; g.memUsedGB = mi[2] / GB;
                    if (nv.temp && nv.temp(nv.dev, 0 /*NVML_TEMPERATURE_GPU*/, &t) == 0) g.tempC = t;
                } else if (nvOk) { NvmlClose(nv); nvOk = false; nvRetry = now + RETRY_MS; }
            } else if (pdhOk) {
                std::wstring luid = a.luid;
                std::vector<std::pair<std::wstring, double>> byType;  // engtype -> summed %
                for (auto& e : eng) {
                    if (e.first.find(luid) == std::wstring::npos) continue;
                    std::wstring t = e.first.substr(e.first.rfind(L"engtype_"));
                    auto f = std::find_if(byType.begin(), byType.end(), [&](auto& p) { return p.first == t; });
                    if (f == byType.end()) byType.push_back({ t, e.second }); else f->second += e.second;
                }
                if (!eng.empty()) {
                    double mx = 0;
                    for (auto& p : byType) mx = std::max(mx, p.second);
                    g.pct = std::min(100.0, mx);
                }
                bool vram = a.dedicatedGB >= 0.25;  // discrete-style adapter shows dedicated, integrated shows shared
                double used = 0; bool seen = false;
                for (auto& e : vram ? ded : shared)
                    if (e.first.find(luid) != std::wstring::npos) { used += e.second; seen = true; }
                if (seen) { g.memUsedGB = used / GB; g.memTotGB = vram ? a.dedicatedGB : a.sharedGB; }
            }
        }

        // drives: system drive first; then every other fixed drive, or only the ones listed in the config
        {
            wchar_t sys[8] = L"C:";
            GetEnvironmentVariableW(L"SystemDrive", sys, 8);
            std::vector<std::wstring> list{ sys };
            std::wstring extra = g_cfg->drives;
            if (extra.empty()) {
                DWORD mask = GetLogicalDrives();
                for (int i = 0; i < 26; i++) {
                    wchar_t d[3] = { (wchar_t)(L'A' + i), L':', 0 }, root[4] = { d[0], L':', L'\\', 0 };
                    if ((mask >> i & 1) && _wcsnicmp(d, sys, 2) != 0 && GetDriveTypeW(root) == DRIVE_FIXED) list.push_back(d);
                }
            }
            for (size_t p = 0; p < extra.size();) {
                size_t c = extra.find(L',', p);
                std::wstring d = extra.substr(p, c == std::wstring::npos ? c : c - p);
                if (d.size() >= 2 && _wcsnicmp(d.c_str(), sys, 2) != 0) list.push_back(d.substr(0, 2));
                p = c == std::wstring::npos ? extra.size() : c + 1;
            }
            for (auto& d : list) {
                ULARGE_INTEGER freeB, total;
                std::wstring root = d + L"\\";
                if (s.ndrives < 4 && GetDiskFreeSpaceExW(root.c_str(), &freeB, &total, nullptr) && total.QuadPart) {
                    DriveSnap& ds = s.drives[s.ndrives++];
                    wcscpy_s(ds.path, d.c_str());
                    ds.totGB = total.QuadPart / GB;
                    ds.usedGB = (total.QuadPart - freeB.QuadPart) / GB;
                }
            }
        }

        // network: per-interface deltas so adding/removing a NIC doesn't spike
        {
            PMIB_IF_TABLE2 t = nullptr;
            if (GetIfTable2(&t) == NO_ERROR) {
                ULONGLONG nowT = GetTickCount64();
                std::vector<NicPrev> cur;
                double din = 0, dout = 0, speed = 0;
                for (ULONG i = 0; i < t->NumEntries; i++) {
                    const MIB_IF_ROW2& r = t->Table[i];
                    if (r.OperStatus != IfOperStatusUp || r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.Type == IF_TYPE_TUNNEL ||
                        VirtualNic(r.Description))
                        continue;
                    cur.push_back({ r.InterfaceLuid.Value, r.InOctets, r.OutOctets });
                    for (auto& p : prev)
                        if (p.luid == r.InterfaceLuid.Value) {  // counter reset (smaller) counts as 0
                            din += r.InOctets >= p.in ? (double)(r.InOctets - p.in) : 0;
                            dout += r.OutOctets >= p.out ? (double)(r.OutOctets - p.out) : 0;
                        }
                    if (r.ReceiveLinkSpeed != ULLONG_MAX) speed += (double)r.ReceiveLinkSpeed;
                }
                FreeMibTable(t);
                if (prevT && nowT > prevT) {
                    double dt = (nowT - prevT) / 1000.0;
                    s.down = din / dt; s.up = dout / dt;
                    if (speed > 0) s.linkPct = std::min(100.0, 100.0 * std::max(s.down, s.up) * 8 / speed);
                }
                prev = cur; prevT = nowT;
            }
        }

        EnterCriticalSection(&g_cs);
        g_snap = s;
        LeaveCriticalSection(&g_cs);
        PostMessageW(g_notify, g_msg, 0, 0);
    } while (WaitForSingleObject(g_stop, (DWORD)std::max(200L, (LONG)InterlockedCompareExchange(&g_cfg->intervalMs, 0, 0))) == WAIT_TIMEOUT);

    if (nvOk) NvmlClose(nv);
    if (pdhOk) PdhCloseQuery(pdh.q);
    return 0;
}

void SamplerStart(SamplerCfg* cfg, HWND notify, UINT msg) {
    InitializeCriticalSection(&g_cs);
    g_cfg = cfg; g_notify = notify; g_msg = msg;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
}

void SamplerStop() {
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
}

Snap SamplerGet() {
    EnterCriticalSection(&g_cs);
    Snap s = g_snap;
    LeaveCriticalSection(&g_cs);
    return s;
}
