#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>

// -1 means "N/A" for every double below.
constexpr double NA = -1;

struct GpuSnap {
    bool present = false;
    int n = 0;                 // DXGI order = GPU number
    wchar_t name[64] = L"";
    bool off = false;          // nvidia_polling disabled
    bool nvidia = false;
    double pct = NA, memUsedGB = NA, memTotGB = NA;
    double tempC = NA;         // NVIDIA only (NVML)
};

struct DriveSnap { wchar_t path[8]; double usedGB, totGB; };

struct Snap {
    double cpu = NA, ramPct = NA, ramUsedGB = NA, ramTotGB = NA;
    GpuSnap gpu[2];
    double diskActive = NA, diskRead = NA, diskWrite = NA;  // MB/s
    DriveSnap drives[4]; int ndrives = 0;
    double down = NA, up = NA, linkPct = NA;                // bytes/s, percent
    double sysTempC = NA;      // hottest ACPI thermal zone; NOT the CPU die temperature
};

struct SamplerCfg {            // read by the sampler thread every loop
    volatile LONG intervalMs = 1000;
    volatile LONG nvidia = 1;
    wchar_t drives[64] = L"";  // empty = every fixed drive; else the system drive plus these (comma separated)
};

void SamplerStart(SamplerCfg* cfg, HWND notify, UINT msg);
void SamplerStop();
Snap SamplerGet();
