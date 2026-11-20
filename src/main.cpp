// Windows System Status Monitor: CPU / RAM / GPU / disk / network / temperatures on the Windows taskbar,
// with a tray-icon fallback.
// Technique (own implementation): a layered child window of Shell_TrayWnd, left of TrayNotifyWnd.
#include "sample.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <string>
#include <vector>

enum Item { CPU, RAM, GPU0, GPU1, DISK, DISKRW, NET, TEMPGPU, TEMPSYS, N_ITEMS };
static const wchar_t* kNames[N_ITEMS] = { L"CPU", L"RAM", L"GPU 0", L"GPU 1", L"Disk active", L"Disk read/write", L"Network",
                                          L"GPU temperature", L"System temperature (ACPI)" };
static const wchar_t* kKeys[N_ITEMS] = { L"show_cpu", L"show_ram", L"show_gpu0", L"show_gpu1", L"show_disk", L"show_disk_rw", L"show_net",
                                         L"show_temp_gpu", L"show_temp_sys" };

static const UINT WM_SAMPLE = WM_APP + 1, WM_TRAYICON = WM_APP + 2, WM_MENU = WM_APP + 3;
static const wchar_t* kMsgClass = L"SystemStatusMonitorMsg";
static const wchar_t* kWidgetClass = L"SystemStatusMonitorWidget";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool g_show[N_ITEMS];
static bool g_showDrive[26];  // per partition letter: show its usage % (config: show_drive_C, ...)
static bool g_bits = false, g_forceTray = false, g_alignLeft = true, g_background = true;  // align=left|right
static int g_offset = 0;  // extra px to shift the widget left (config: offset)
static SamplerCfg g_sc;
static wchar_t g_ini[MAX_PATH];

static HINSTANCE g_inst;
static HWND g_msgWnd, g_widget, g_bar, g_tip;
static Snap g_snap;
static SIZE g_size;
static UINT g_taskbarCreated;
static bool g_trayAdded;
static std::wstring g_tipText;

// ---- config (INI in %APPDATA%\WindowsSystemStatusMonitor) ----------------------------------------------------------
static void LoadConfig() {
    wchar_t dir[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    wcscat_s(dir, L"\\WindowsSystemStatusMonitor");
    CreateDirectoryW(dir, nullptr);
    swprintf_s(g_ini, L"%s\\config.ini", dir);
    for (int i = 0; i < N_ITEMS; i++) g_show[i] = GetPrivateProfileIntW(L"monitor", kKeys[i], 1, g_ini) != 0;
    g_sc.intervalMs = GetPrivateProfileIntW(L"monitor", L"interval_ms", 1000, g_ini);
    g_sc.nvidia = GetPrivateProfileIntW(L"monitor", L"nvidia_polling", 1, g_ini);
    g_bits = GetPrivateProfileIntW(L"monitor", L"net_bits", 0, g_ini) != 0;
    g_offset = GetPrivateProfileIntW(L"monitor", L"offset", 0, g_ini);
    GetPrivateProfileStringW(L"monitor", L"drives", L"", g_sc.drives, 64, g_ini);
    wchar_t al[16];
    GetPrivateProfileStringW(L"monitor", L"align", L"left", al, 16, g_ini);
    g_alignLeft = _wcsicmp(al, L"right") != 0;
    g_background = GetPrivateProfileIntW(L"monitor", L"background", 1, g_ini) != 0;
    for (int i = 0; i < 26; i++) {
        wchar_t k[24];
        swprintf_s(k, L"show_drive_%c", L'A' + i);
        g_showDrive[i] = GetPrivateProfileIntW(L"monitor", k, 1, g_ini) != 0;
    }
}

static void SaveInt(const wchar_t* key, int v) {
    wchar_t b[16];
    swprintf_s(b, L"%d", v);
    WritePrivateProfileStringW(L"monitor", key, b, g_ini);
}

static void SaveConfig() {
    for (int i = 0; i < N_ITEMS; i++) SaveInt(kKeys[i], g_show[i]);
    SaveInt(L"interval_ms", g_sc.intervalMs);
    SaveInt(L"nvidia_polling", g_sc.nvidia);
    SaveInt(L"net_bits", g_bits);
    SaveInt(L"offset", g_offset);
    SaveInt(L"background", g_background);
    WritePrivateProfileStringW(L"monitor", L"drives", g_sc.drives, g_ini);
    WritePrivateProfileStringW(L"monitor", L"align", g_alignLeft ? L"left" : L"right", g_ini);
    for (int i = 0; i < g_snap.ndrives; i++) {  // only partitions we know about, to keep the file tidy
        wchar_t k[24];
        int idx = towupper(g_snap.drives[i].path[0]) - L'A';
        if (idx < 0 || idx >= 26) continue;
        swprintf_s(k, L"show_drive_%c", L'A' + idx);
        SaveInt(k, g_showDrive[idx]);
    }
}

// ---- start with Windows (state is read from the registry, not config) -----------------------------
static std::wstring ExeCommand() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::wstring(L"\"") + p + L"\"";
}

static bool AutostartEnabled() {
    wchar_t v[MAX_PATH + 4];
    DWORD sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"WindowsSystemStatusMonitor", RRF_RT_REG_SZ, nullptr, v, &sz) == ERROR_SUCCESS && ExeCommand() == v;
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring c = ExeCommand();
        RegSetValueExW(k, L"WindowsSystemStatusMonitor", 0, REG_SZ, (const BYTE*)c.c_str(), (DWORD)((c.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"WindowsSystemStatusMonitor");
    }
    RegCloseKey(k);
}

// ---- text model: columns of up to two stacked rows; each row is a few label+value cells --------------------
struct Cell {
    std::wstring label, value;
    const wchar_t* slot;  // widest expected value: keeps the cell width constant so nothing jumps
    int level;            // -1 plain, 0 green, 1 orange, 2 red
    wchar_t glyph = 0;    // icon-font glyph drawn instead of `label` when available (else `label` is drawn)
    std::wstring suffix;  // text drawn right after the glyph (e.g. the GPU number)
};
typedef std::vector<Cell> Row;
typedef std::vector<Row> Col;

static int Level(double p) { return p < 60 ? 0 : p <= 85 ? 1 : 2; }

static std::wstring Fmt(const wchar_t* f, double v) {
    wchar_t b[32];
    swprintf_s(b, f, v);
    return b;
}

static Cell Pct(const std::wstring& label, double p, bool off = false) {
    return { label, off ? L"off" : p < 0 ? L"N/A" : Fmt(L"%.0f%%", p), L"99%", (off || p < 0) ? -1 : Level(p) };
}

static Cell Temp(const wchar_t* label, double t, double warn, double hot, bool off = false) {
    return { label, off ? L"off" : t < 0 ? L"N/A" : Fmt(L"%.0f°C", t), L"99°C", (off || t < 0) ? -1 : t < warn ? 0 : t <= hot ? 1 : 2 };
}

static Cell WithGlyph(Cell c, wchar_t glyph, const std::wstring& suffix = L"") {
    c.glyph = glyph;
    c.suffix = suffix;
    return c;
}

static std::wstring Rate(double b) {
    if (b < 0) return L"N/A";
    if (g_bits) {
        double kb = b * 8 / 1000;
        return kb < 1000 ? Fmt(L"%.0fKb", kb) : Fmt(L"%.1fMb", kb / 1000);
    }
    double k = b / 1024;
    return k < 1000 ? Fmt(L"%.0fK", k) : Fmt(L"%.1fM", k / 1024);
}

static std::wstring ShortName(const wchar_t* n) {
    std::wstring s = n;
    for (const wchar_t* k : { L"(R)", L"(TM)", L"NVIDIA", L"GeForce", L"Laptop GPU", L"Graphics" }) {
        size_t p;
        while ((p = s.find(k)) != std::wstring::npos) s.erase(p, wcslen(k));
    }
    size_t a = s.find_first_not_of(L' '), b = s.find_last_not_of(L' ');
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// Columns in priority order: when space runs out the last ones are dropped first.
static std::vector<Col> BuildCols(const Snap& s) {
    std::vector<Col> cols;
    auto add = [&](Col c) { if (!c.empty()) cols.push_back(c); };
    Col c1, c2, c3, c4, c5;
    if (g_show[CPU]) c1.push_back({ WithGlyph(Pct(L"CPU", s.cpu), 0xE950) });
    if (g_show[RAM]) c1.push_back({ WithGlyph(Pct(L"RAM", s.ramPct), 0xEEA0) });
    for (int i = 0; i < 2; i++)
        if (g_show[GPU0 + i] && s.gpu[i].present) {
            wchar_t l[8];
            swprintf_s(l, L"GPU%d", s.gpu[i].n);
            c2.push_back({ WithGlyph(Pct(l, s.gpu[i].pct, s.gpu[i].off), 0xE990, std::to_wstring(s.gpu[i].n)) });
        }
    if (g_show[DISK]) c3.push_back({ WithGlyph(Pct(L"DSK", s.diskActive), 0xEE94) });
    if (g_show[DISKRW])
        c3.push_back({ { L"R", s.diskRead < 0 ? L"N/A" : Fmt(L"%.1f", s.diskRead), L"9.9", -1 },
                       { L"W", s.diskWrite < 0 ? L"N/A" : Fmt(L"%.1f", s.diskWrite), L"9.9", -1 } });
    add(c1); add(c2); add(c3);
    Col parts;  // partition usage %, two per column
    for (int i = 0; i < s.ndrives; i++) {
        int idx = towupper(s.drives[i].path[0]) - L'A';
        if (idx < 0 || idx >= 26 || !g_showDrive[idx] || s.drives[i].totGB <= 0) continue;
        parts.push_back({ Pct(s.drives[i].path, 100.0 * s.drives[i].usedGB / s.drives[i].totGB) });
        if (parts.size() == 2) { add(parts); parts.clear(); }
    }
    add(parts);
    if (g_show[NET]) {
        const wchar_t* slot = g_bits ? L"999Kb" : L"999K";
        c4.push_back({ { L"↓", Rate(s.down), slot, -1 } });
        c4.push_back({ { L"↑", Rate(s.up), slot, -1 } });
    }
    add(c4);
    if (g_show[TEMPGPU])
        for (auto& g : s.gpu)
            if (g.present && g.nvidia) c5.push_back({ WithGlyph(Temp(L"GPU", g.tempC, 70, 85, g.off), 0xE9CA, L"GPU") });
    if (g_show[TEMPSYS]) c5.push_back({ WithGlyph(Temp(L"SYS", s.sysTempC, 50, 65), 0xE9CA, L"SYS") });
    add(c5);  // lowest priority: dropped first when space runs out
    return cols;
}

static std::wstring DetailText(const Snap& s) {
    std::wstring t = L"CPU " + (s.cpu < 0 ? L"N/A" : Fmt(L"%.0f%%", s.cpu));
    t += L"\nRAM " + (s.ramPct < 0 ? L"N/A" : Fmt(L"%.1f", s.ramUsedGB) + L" / " + Fmt(L"%.1f", s.ramTotGB) + L" GB (" + Fmt(L"%.0f%%", s.ramPct) + L")");
    for (auto& g : s.gpu) {
        if (!g.present) continue;
        wchar_t l[16];
        swprintf_s(l, L"\nGPU %d · ", g.n);
        t += l + ShortName(g.name) + L": ";
        if (g.off) { t += L"polling off"; continue; }
        t += g.pct < 0 ? L"N/A" : Fmt(L"%.0f%%", g.pct);
        if (g.memTotGB > 0) t += L" · mem " + Fmt(L"%.1f", g.memUsedGB) + L" / " + Fmt(L"%.1f", g.memTotGB) + L" GB";
        if (g.tempC >= 0) t += L" · " + Fmt(L"%.0f", g.tempC) + L"°C";
    }
    if (s.sysTempC >= 0) t += L"\nSystem (ACPI) " + Fmt(L"%.0f", s.sysTempC) + L"°C";
    t += L"\nDisk " + (s.diskActive < 0 ? L"N/A" : Fmt(L"%.0f%%", s.diskActive) + L" active · R " + Fmt(L"%.1f", s.diskRead) + L" W " + Fmt(L"%.1f", s.diskWrite) + L" MB/s");
    for (int i = 0; i < s.ndrives; i++)
        t += L"\n" + std::wstring(s.drives[i].path) + L" " + Fmt(L"%.1f", s.drives[i].usedGB) + L" / " + Fmt(L"%.1f", s.drives[i].totGB) + L" GB";
    t += L"\nNet ↓ " + Rate(s.down) + L"  ↑ " + Rate(s.up) + (s.linkPct >= 0 ? L"  (link " + Fmt(L"%.1f%%", s.linkPct) + L")" : L"");
    return t;
}

// ---- drawing -----------------------------------------------------------------------------------------
struct Rgb { int r, g, b; };

static UINT DpiOf(HWND w) {
    UINT d = w ? GetDpiForWindow(w) : 0;
    return d ? d : 96;
}

static int Tw(HDC dc, HFONT f, const std::wstring& t) {
    SIZE sz;
    SelectObject(dc, f);
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    return sz.cx;
}

static HFONT g_fl, g_fv, g_fi;  // label, value, icon
static UINT g_fdpi;
static const wchar_t* g_iconFace;  // nullptr when neither icon font is installed

static int CALLBACK FontFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM l) {
    *(bool*)l = true;
    return 0;
}

static bool FontInstalled(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, FontFound, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static void EnsureFonts(UINT dpi) {
    if (g_fdpi == dpi) return;
    if (g_fl) DeleteObject(g_fl);
    if (g_fv) DeleteObject(g_fv);
    if (g_fi) DeleteObject(g_fi);
    g_iconFace = FontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : FontInstalled(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr;
    g_fl = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    g_fv = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    g_fi = g_iconFace ? CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, g_iconFace)
                      : nullptr;
    g_fdpi = dpi;
}

// A glyph the installed icon font lacks would show as an empty box: those cells fall back to their text label.
static bool HasGlyph(HDC dc, wchar_t g) {
    WORD idx = 0xFFFF;
    SelectObject(dc, g_fi);
    return GetGlyphIndicesW(dc, &g, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && idx != 0xFFFF;
}

struct CellBox { int x, lw, vw, gw; bool icon; };

// Premultiplied ARGB "source over".
static DWORD Over(DWORD dst, int sa, int sr, int sg, int sb) {
    int k = 255 - sa;
    int a = sa + (int)(dst >> 24) * k / 255, r = sr + (int)((dst >> 16) & 255) * k / 255;
    int g = sg + (int)((dst >> 8) & 255) * k / 255, b = sb + (int)(dst & 255) * k / 255;
    return ((DWORD)std::min(a, 255) << 24) | ((DWORD)std::min(r, 255) << 16) | ((DWORD)std::min(g, 255) << 8) | (DWORD)std::min(b, 255);
}

// Draws the widget into a layered bitmap at most `avail` px wide. Returns false (and draws nothing) if even
// the first column does not fit.
static bool Paint(int avail, int H, UINT dpi) {
    auto S = [&](int x) { return MulDiv(x, dpi, 96); };
    EnsureFonts(dpi);
    std::vector<Col> cols = BuildCols(g_snap);
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);

    std::vector<std::vector<std::vector<CellBox>>> box;  // [col][row][cell]
    std::vector<int> colStart;
    int W = 0;
    const int pad = S(10);
    for (;;) {
        box.assign(cols.size(), {});
        colStart.assign(cols.size(), 0);
        int x = pad;
        for (size_t c = 0; c < cols.size(); c++) {
            int colW = 0;
            colStart[c] = x;
            for (auto& row : cols[c]) {
                box[c].emplace_back();
                int rx = 0;
                for (auto& cell : row) {
                    bool icon = cell.glyph && g_fi && HasGlyph(mem, cell.glyph);
                    int gw = icon ? Tw(mem, g_fi, std::wstring(1, cell.glyph)) : 0;
                    int lw = icon ? gw + (cell.suffix.empty() ? 0 : S(2) + Tw(mem, g_fl, cell.suffix)) : Tw(mem, g_fl, cell.label);
                    int vw = std::max(Tw(mem, g_fv, cell.slot), Tw(mem, g_fv, cell.value));
                    box[c].back().push_back({ x + rx, lw, vw, gw, icon });
                    rx += lw + S(4) + vw + S(6);
                }
                colW = std::max(colW, rx - S(6));
            }
            x += colW + S(12);
        }
        W = x - S(12) + pad;
        if (W <= avail || cols.size() <= 1) break;
        cols.pop_back();
    }
    if (cols.empty() || W > avail) { DeleteDC(mem); ReleaseDC(nullptr, scr); return false; }

    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    DWORD* px = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));  // white-on-black coverage mask; colour, shadow and background are composed below

    // Labels and icons are always white; only threshold values are coloured.
    const Rgb white{ 255, 255, 255 };
    const Rgb lv[3] = { { 74, 222, 128 }, { 251, 146, 60 }, { 248, 113, 113 } };
    struct Region { RECT r; Rgb c; };
    std::vector<Region> regions;
    const UINT fl = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    int rh = S(16);
    for (size_t c = 0; c < cols.size(); c++) {
        int n = (int)cols[c].size();
        int y0 = (H - n * rh) / 2;
        for (int r = 0; r < n; r++)
            for (size_t k = 0; k < cols[c][r].size(); k++) {
                const Cell& cell = cols[c][r][k];
                const CellBox& b = box[c][r][k];
                int y = y0 + r * rh;
                RECT rl{ b.x, y, b.x + b.lw, y + rh }, rv{ b.x + b.lw + S(4), y, b.x + b.lw + S(4) + b.vw, y + rh };
                if (b.icon) {
                    RECT rg{ b.x, y, b.x + b.gw, y + rh };
                    std::wstring gs(1, cell.glyph);
                    SelectObject(mem, g_fi);
                    DrawTextW(mem, gs.c_str(), 1, &rg, fl | DT_LEFT);
                    if (!cell.suffix.empty()) {
                        RECT rs{ b.x + b.gw + S(2), y, b.x + b.lw, y + rh };
                        SelectObject(mem, g_fl);
                        DrawTextW(mem, cell.suffix.c_str(), -1, &rs, fl | DT_LEFT);
                    }
                } else {
                    SelectObject(mem, g_fl);
                    DrawTextW(mem, cell.label.c_str(), -1, &rl, fl | DT_LEFT);
                }
                SelectObject(mem, g_fv);
                DrawTextW(mem, cell.value.c_str(), -1, &rv, fl | DT_RIGHT);
                regions.push_back({ rl, white });
                regions.push_back({ rv, cell.level < 0 ? white : lv[cell.level] });
            }
    }
    for (auto& g : regions)  // coverage -> premultiplied ARGB text layer (in place)
        for (int yy = std::max<LONG>(0, g.r.top); yy < std::min<LONG>(H, g.r.bottom); yy++)
            for (int xx = std::max<LONG>(0, g.r.left); xx < std::min<LONG>(W, g.r.right); xx++) {
                DWORD a = (px[yy * W + xx] >> 8) & 0xFF;
                px[yy * W + xx] = (a << 24) | ((g.c.r * a / 255) << 16) | ((g.c.g * a / 255) << 8) | (g.c.b * a / 255);
            }

    // Background layer: dark translucent rounded pill with a faint border and column dividers (optional).
    std::vector<DWORD> bg((size_t)W * H, 0);
    if (g_background) {
        int top = S(4), bot = H - S(4);
        double rad = S(8), cx = W / 2.0, cy = (top + bot) / 2.0, hw = W / 2.0, hh = (bot - top) / 2.0;
        for (int yy = top; yy < bot; yy++)
            for (int xx = 0; xx < W; xx++) {
                double qx = fabs(xx + 0.5 - cx) - (hw - rad), qy = fabs(yy + 0.5 - cy) - (hh - rad);
                double d = hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - rad;  // < 0 inside
                double cover = std::min(1.0, std::max(0.0, 0.5 - d));
                if (cover <= 0) continue;
                int fa = (int)(140 * cover);                                               // fill: rgba(20,20,20,0.55)
                DWORD p = ((DWORD)fa << 24) | ((DWORD)(20 * fa / 255) << 16) | ((DWORD)(20 * fa / 255) << 8) | (DWORD)(20 * fa / 255);
                int ba = (int)(22 * cover * std::min(1.0, std::max(0.0, 1.0 + d)));       // 1px border, white ~8%
                bg[yy * W + xx] = ba ? Over(p, ba, ba, ba, ba) : p;
            }
        for (size_t c = 1; c < cols.size(); c++) {
            int lx = colStart[c] - S(6);
            for (int yy = H * 20 / 100; yy < H * 80 / 100; yy++)
                if (lx >= 0 && lx < W) bg[yy * W + lx] = Over(bg[yy * W + lx], 38, 38, 38, 38);
        }
    }

    // Compose: background, then a 1px soft shadow of the text, then the text. Alpha is kept >= 1 everywhere:
    // fully transparent pixels let the mouse through, alpha 1 is invisible but keeps the strip hoverable/clickable.
    std::vector<DWORD> out((size_t)W * H);
    for (int yy = 0; yy < H; yy++)
        for (int xx = 0; xx < W; xx++) {
            DWORD o = bg[yy * W + xx];
            if (xx > 0 && yy > 0) {
                int sa = (int)(px[(yy - 1) * W + xx - 1] >> 24) * 128 / 255;
                if (sa) o = Over(o, sa, 0, 0, 0);
            }
            DWORD t = px[yy * W + xx];
            if (t >> 24) o = Over(o, t >> 24, (t >> 16) & 255, (t >> 8) & 255, t & 255);
            out[yy * W + xx] = (o >> 24) ? o : 0x01000000;
        }
    memcpy(px, out.data(), (size_t)W * H * sizeof(DWORD));

    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_widget, scr, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    g_size = sz;
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    return true;
}

// ---- taskbar widget -------------------------------------------------------------------------------------
static LRESULT CALLBACK WidgetProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_tip && (m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_RBUTTONDOWN || m == WM_RBUTTONUP)) {
        MSG relay{ h, m, w, l };  // TTF_SUBCLASS does not work for this window, so relay by hand
        SendMessageW(g_tip, TTM_RELAYEVENT, 0, (LPARAM)&relay);
    }
    switch (m) {
    case WM_RBUTTONUP: PostMessageW(g_msgWnd, WM_MENU, 0, 0); return 0;
    case WM_NOTIFY:
        if (((NMHDR*)l)->code == TTN_GETDISPINFOW) {
            g_tipText = DetailText(g_snap);
            ((NMTTDISPINFOW*)l)->lpszText = (LPWSTR)g_tipText.c_str();
            return 0;
        }
        break;
    case WM_NCHITTEST: return HTCLIENT;
    }
    return DefWindowProcW(h, m, w, l);
}

// Creating the window directly as a child of Shell_TrayWnd can fail on some Windows 11 builds
// (ERROR_MOD_NOT_FOUND); a popup re-parented afterwards works.
static HWND CreateWidget(HWND bar) {
    HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kWidgetClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                             nullptr, g_inst, nullptr);
    if (!w) return nullptr;
    SetWindowLongPtrW(w, GWL_STYLE, (GetWindowLongPtrW(w, GWL_STYLE) & ~WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS);
    if (!SetParent(w, bar)) { DestroyWindow(w); return nullptr; }
    return w;
}

static void DestroyWidget() {
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
    if (g_widget && IsWindow(g_widget)) DestroyWindow(g_widget);
    g_widget = nullptr;
}

static bool FindBar(HWND& bar, HWND& tray) {
    bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    tray = bar ? FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT r;
    // vertical taskbars have no room for a text strip -> tray fallback
    return bar && tray && IsWindowVisible(tray) && GetWindowRect(bar, &r) && r.right - r.left > r.bottom - r.top;
}

struct Others { HWND self; std::vector<RECT> r; };

static BOOL CALLBACK CollectOthers(HWND c, LPARAM l) {
    auto* o = (Others*)l;
    RECT r;
    wchar_t cls[64];
    GetClassNameW(c, cls, 64);
    // other layered widgets (e.g. another taskbar tool); skip the XAML island and Explorer's own windows
    if (c != o->self && IsWindowVisible(c) && (GetWindowLongW(c, GWL_EXSTYLE) & WS_EX_LAYERED) && wcsncmp(cls, L"Windows.UI", 10) != 0 &&
        GetWindowRect(c, &r))
        o->r.push_back(r);
    return TRUE;
}

// Right edge of Start / Search / task buttons. On Windows 11 they are XAML elements without an HWND, so
// UI Automation is the only way to see them. Runs on its own thread: UIA also walks our own widget window, so
// calling it from the UI thread deadlocks. A full FindAll costs ~30 ms inside Explorer, hence every 10 s.
static const LONG kNotMeasured = LONG_MAX;  // "unknown yet" (INT_MIN = measuring failed -> assume free)
static volatile LONG g_btnRight = kNotMeasured;  // right edge of the button cluster (INT_MIN = none found)
static volatile LONG g_btnLeft = INT_MAX;        // left edge of the button cluster (INT_MAX = none found)
static HANDLE g_quit;
static HANDLE g_uiaThread;

static void MeasureButtons(IUIAutomation* ua, HWND bar, int trayLeft, int barLeft, LONG& outLeft, LONG& outRight) {
    LONG cached = INT_MIN, leftMost = INT_MAX;
    IUIAutomation* g_ua = ua;
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* cond = nullptr;
    IUIAutomationElementArray* arr = nullptr;
    if (SUCCEEDED(g_ua->ElementFromHandle(bar, &root)) && SUCCEEDED(g_ua->CreateTrueCondition(&cond)) &&
        SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
        int n = 0;
        arr->get_Length(&n);
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* e = nullptr;
            if (FAILED(arr->GetElement(i, &e)) || !e) continue;
            CONTROLTYPEID ct = 0;
            UIA_HWND hw = nullptr;
            RECT r{};
            BSTR cls = nullptr;
            e->get_CurrentControlType(&ct);
            e->get_CurrentNativeWindowHandle(&hw);
            e->get_CurrentBoundingRectangle(&r);
            e->get_CurrentClassName(&cls);
            bool tray = cls && wcsncmp(cls, L"SystemTray", 10) == 0;
            if (ct == UIA_ButtonControlTypeId && !hw && !tray && r.right > r.left && r.left >= barLeft && r.right <= trayLeft) {
                cached = std::max(cached, r.right);
                leftMost = std::min(leftMost, r.left);
            }
            SysFreeString(cls);
            e->Release();
        }
    }
    if (arr) arr->Release();
    if (cond) cond->Release();
    if (root) root->Release();
    outLeft = leftMost;
    outRight = cached;
}

static DWORD WINAPI UiaThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* ua = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&ua))) ua = nullptr;
    do {
        HWND bar, tray;
        RECT br, tr;
        LONG l = INT_MAX, v = INT_MIN;
        if (ua && FindBar(bar, tray) && GetWindowRect(bar, &br) && GetWindowRect(tray, &tr)) MeasureButtons(ua, bar, tr.left, br.left, l, v);
        InterlockedExchange(&g_btnLeft, l);
        InterlockedExchange(&g_btnRight, v);  // written last: Layout waits on this one for "measured"
    } while (WaitForSingleObject(g_quit, 10000) == WAIT_TIMEOUT);
    if (ua) ua->Release();
    CoUninitialize();
    return 0;
}

// Paints at the width that fits and places the widget left of the tray (and of other layered widgets),
// right of the task buttons. Hides it when nothing fits.
static void Layout(HWND bar, HWND tray) {
    RECT br, tr;
    GetWindowRect(bar, &br);
    GetWindowRect(tray, &tr);
    UINT dpi = DpiOf(bar);
    auto S = [&](int x) { return MulDiv(x, dpi, 96); };
    // shortcut: other widgets are found only if they are layered children of the taskbar
    Others o{ g_widget, {} };
    EnumChildWindows(bar, CollectOthers, (LPARAM)&o);
    int right = tr.left;
    for (int pass = 0; pass < 4; pass++)
        for (auto& r : o.r)
            if (r.right > right - S(4) && r.left < right && r.right - r.left < (br.right - br.left) / 2) right = r.left;
    right -= S(2) + S(g_offset);
    LONG btn = g_btnRight, btnLeft = g_btnLeft;
    if (btn == kNotMeasured) { ShowWindow(g_widget, SW_HIDE); return; }  // wait for the first measurement
    int minLeft = btn == INT_MIN ? br.left : btn + S(8);
    int H = br.bottom - br.top;
    // Preferred side first (config align), the other side as fallback: e.g. a left-aligned taskbar has no room on
    // the left, so the widget goes right instead of vanishing. Paint() draws nothing when it returns false.
    int x = 0;
    bool placed = false;
    for (int pass = 0; pass < 2 && !placed; pass++) {
        bool left = (pass == 0) == g_alignLeft;
        if (left) {
            int x0 = br.left + S(8) + S(g_offset), avail = btnLeft - S(8) - x0;
            if (btnLeft != INT_MAX && avail > 0 && Paint(avail, H, dpi)) { x = x0; placed = true; }
        } else if (Paint(right - minLeft, H, dpi)) {
            x = right - g_size.cx;
            placed = true;
        }
    }
    if (!placed) { ShowWindow(g_widget, SW_HIDE); return; }
    POINT p{ x, br.top };
    ScreenToClient(bar, &p);
    SetWindowPos(g_widget, HWND_TOP, p.x, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void MakeTooltip() {
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, g_msgWnd, nullptr, g_inst, nullptr);
    TOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_IDISHWND;
    ti.hwnd = g_widget;
    ti.uId = (UINT_PTR)g_widget;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
}

// ---- tray fallback ----------------------------------------------------------------------------------------
static HICON MakeIcon(double cpu) {
    int n = GetSystemMetrics(SM_CXSMICON);
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB };
    DWORD* px = nullptr;
    HBITMAP color = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, color);
    COLORREF bg = cpu < 0 ? RGB(90, 90, 90) : cpu < 60 ? RGB(30, 130, 70) : cpu <= 85 ? RGB(190, 100, 20) : RGB(190, 45, 45);
    HBRUSH br = CreateSolidBrush(bg);
    RECT r{ 0, 0, n, n };
    FillRect(mem, &r, br);
    DeleteObject(br);
    HFONT f = CreateFontW(-n * 3 / 4, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    SelectObject(mem, f);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    wchar_t t[8] = L"--";
    if (cpu >= 0) swprintf_s(t, L"%d", (int)std::min(99.0, cpu + 0.5));
    DrawTextW(mem, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    for (int i = 0; i < n * n; i++) px[i] |= 0xFF000000;  // opaque
    SelectObject(mem, old);
    DeleteObject(f);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    return ic;
}

static NOTIFYICONDATAW TrayData() {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    return nid;
}

static void UpdateTray() {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = MakeIcon(g_snap.cpu);
    std::wstring tip;
    for (auto& col : BuildCols(g_snap))
        for (auto& row : col) {
            for (auto& c : row) tip += c.label + L" " + c.value + L" ";
            tip += L" ";
        }
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(g_trayAdded ? NIM_MODIFY : NIM_ADD, &nid);
    g_trayAdded = true;
    DestroyIcon(nid.hIcon);
}

static void RemoveTray() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid = TrayData();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// ---- mode selection (runs on a timer, so Explorer restarts and taskbar changes are picked up) -------------
static void Refresh() {
    HWND bar, tray;
    bool ok = !g_forceTray && FindBar(bar, tray);
    if (ok && (!g_widget || !IsWindow(g_widget) || GetParent(g_widget) != bar)) {
        DestroyWidget();
        g_widget = CreateWidget(bar);
        if (g_widget) MakeTooltip();
    }
    if (ok && g_widget) {
        g_bar = bar;
        RemoveTray();
        Layout(bar, tray);
    } else {
        DestroyWidget();
        g_bar = nullptr;
        UpdateTray();
    }
}

// ---- menu ------------------------------------------------------------------------------------------------------
enum { ID_ITEM = 100, ID_INT = 200, ID_BYTES = 300, ID_BITS = 301, ID_NV = 400, ID_AUTO = 410, ID_BG = 420, ID_EXIT = 499, ID_DRIVE = 600 };
static const int kIntervals[] = { 500, 1000, 2000, 5000 };

static void ShowMenu() {
    HMENU m = CreatePopupMenu(), si = CreatePopupMenu(), sn = CreatePopupMenu();
    for (int i = 0; i < N_ITEMS; i++) {
        AppendMenuW(m, MF_STRING | (g_show[i] ? MF_CHECKED : 0), ID_ITEM + i, kNames[i]);
        if (i != DISKRW) continue;
        for (int d = 0; d < g_snap.ndrives; d++) {  // one switch per partition, right after the disk items
            int idx = towupper(g_snap.drives[d].path[0]) - L'A';
            if (idx < 0 || idx >= 26) continue;
            std::wstring t = L"Partition " + std::wstring(g_snap.drives[d].path) + L" usage";
            AppendMenuW(m, MF_STRING | (g_showDrive[idx] ? MF_CHECKED : 0), ID_DRIVE + idx, t.c_str());
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < 4; i++) {
        wchar_t t[16];
        swprintf_s(t, L"%g s", kIntervals[i] / 1000.0);
        AppendMenuW(si, MF_STRING | (g_sc.intervalMs == kIntervals[i] ? MF_CHECKED : 0), ID_INT + i, t);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)si, L"Sample interval");
    AppendMenuW(sn, MF_STRING | (!g_bits ? MF_CHECKED : 0), ID_BYTES, L"Bytes (KB/s, MB/s)");
    AppendMenuW(sn, MF_STRING | (g_bits ? MF_CHECKED : 0), ID_BITS, L"Bits (Kbps, Mbps)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sn, L"Network unit");
    AppendMenuW(m, MF_STRING | (g_sc.nvidia ? MF_CHECKED : 0), ID_NV, L"Poll NVIDIA GPU");
    AppendMenuW(m, MF_STRING | (g_background ? MF_CHECKED : 0), ID_BG, L"Background");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTO, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_msgWnd);  // required or the menu never closes on outside click
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_msgWnd, nullptr);
    PostMessageW(g_msgWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void OnCommand(int id) {
    if (id >= ID_ITEM && id < ID_ITEM + N_ITEMS) g_show[id - ID_ITEM] = !g_show[id - ID_ITEM];
    else if (id >= ID_DRIVE && id < ID_DRIVE + 26) g_showDrive[id - ID_DRIVE] = !g_showDrive[id - ID_DRIVE];
    else if (id >= ID_INT && id < ID_INT + 4) g_sc.intervalMs = kIntervals[id - ID_INT];
    else if (id == ID_BYTES || id == ID_BITS) g_bits = id == ID_BITS;
    else if (id == ID_NV) g_sc.nvidia = !g_sc.nvidia;
    else if (id == ID_BG) g_background = !g_background;
    else if (id == ID_AUTO) SetAutostart(!AutostartEnabled());
    else if (id == ID_EXIT) { DestroyWindow(g_msgWnd); return; }
    SaveConfig();
    Refresh();
}

static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated) { g_trayAdded = false; Refresh(); return 0; }
    switch (m) {
    case WM_SAMPLE: g_snap = SamplerGet(); Refresh(); return 0;
    case WM_TIMER: Refresh(); return 0;
    case WM_MENU: ShowMenu(); return 0;
    case WM_COMMAND: OnCommand(LOWORD(w)); return 0;
    case WM_TRAYICON:
        if (LOWORD(l) == WM_RBUTTONUP) ShowMenu();
        return 0;
    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE: Refresh(); return 0;
    case WM_DESTROY:
        RemoveTray();
        DestroyWidget();
        SamplerStop();
        SetEvent(g_quit);
        WaitForSingleObject(g_uiaThread, 3000);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmd, int) {
    g_inst = hi;
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"WindowsSystemStatusMonitor_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_forceTray = wcsstr(cmd, L"--tray") != nullptr;
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    LoadConfig();

    WNDCLASSW wc{};
    wc.hInstance = hi;
    wc.lpfnWndProc = MsgProc;
    wc.lpszClassName = kMsgClass;
    RegisterClassW(&wc);
    wc.lpfnWndProc = WidgetProc;
    wc.lpszClassName = kWidgetClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_msgWnd = CreateWindowExW(0, kMsgClass, L"System Status Monitor", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);  // hidden top-level: receives broadcasts
    SetTimer(g_msgWnd, 1, 2000, nullptr);
    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_uiaThread = CreateThread(nullptr, 0, UiaThread, nullptr, 0, nullptr);
    SamplerStart(&g_sc, g_msgWnd, WM_SAMPLE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CloseHandle(mtx);
    return 0;
}
