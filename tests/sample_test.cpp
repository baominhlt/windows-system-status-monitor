// Console check of the sampler: prints 5 snapshots. Build: see build.bat (test target).
#include <cstdio>

#include "sample.h"

int wmain(int argc, wchar_t** argv) {
    static SamplerCfg cfg;
    if (argc > 1) cfg.nvidia = 0;
    HWND h = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    SamplerStart(&cfg, h, WM_APP);
    Sleep(1500);
    for (int i = 0; i < 5; i++) {
        Sleep(1000);
        Snap s = SamplerGet();
        wprintf(L"cpu %.1f | ram %.1f%% %.1f/%.1f GB | disk act %.1f%% R %.2f W %.2f MB/s | net down %.0f up %.0f B/s link %.2f%%\n", s.cpu,
                s.ramPct, s.ramUsedGB, s.ramTotGB, s.diskActive, s.diskRead, s.diskWrite, s.down, s.up, s.linkPct);
        for (auto& g : s.gpu)
            if (g.present) wprintf(L"  GPU%d %s off=%d %.1f%% mem %.2f/%.2f GB temp %.0f C\n", g.n, g.name, g.off, g.pct, g.memUsedGB, g.memTotGB, g.tempC);
        wprintf(L"  sys(ACPI) temp %.1f C\n", s.sysTempC);
        for (int d = 0; d < s.ndrives; d++) wprintf(L"  %s %.1f/%.1f GB\n", s.drives[d].path, s.drives[d].usedGB, s.drives[d].totGB);
    }
    SamplerStop();
    return 0;
}
