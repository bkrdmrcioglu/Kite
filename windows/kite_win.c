/*
 * Kite for Windows: system tray app around kite-core.
 *
 * - runs the engine (HTTP/SOCKS5 proxy on 127.0.0.1:8899, TLS on 127.0.0.1:443)
 * - points the per-user WinINet proxy at it (no admin needed) and restores the
 *   previous settings on exit, shutdown, or the next launch after a crash
 * - optional Discord app mode: hosts-file entries, written by an elevated copy
 *   of this exe started with --hosts-install / --hosts-remove
 * - a Windows 11 style flyout panel opened from the tray icon
 */
#define _WIN32_WINNT 0x0A00
#include "compat.h" /* first: winsock2.h must precede windows.h */
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include <wininet.h>
#include "kite.h"

#define PROXY_PORT 8899
#define SNI_PORT 443
#define WM_TRAY (WM_APP + 1)
#define ID_OPEN 1000
#define ID_TOGGLE 1001
#define ID_QUIT 1004
#define IDI_KITE_ON 1
#define IDI_KITE_OFF 2

static const wchar_t *INET_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
static const wchar_t *BACKUP_KEY = L"Software\\Kite\\ProxyBackup";
static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char *HOSTS_BEGIN = "# >>> Kite (Discord) >>>";
static const char *HOSTS_END = "# <<< Kite (Discord) <<<";

static const char *discord_domains[] = {
    "discord.com", "www.discord.com", "ptb.discord.com", "canary.discord.com",
    "updates.discord.com", "status.discord.com", "cdn.discord.com",
    "discordapp.com", "www.discordapp.com", "cdn.discordapp.com",
    "media.discordapp.net", "images-ext-1.discordapp.net", "images-ext-2.discordapp.net",
    "dl.discordapp.net", "router.discordapp.net",
    "discord.gg", "gateway.discord.gg", "discord.media", "discord.new",
    "discordstatus.com", NULL
};

static HWND g_hwnd;   /* hidden message window owning the tray icon */
static HWND g_panel;  /* flyout */
static NOTIFYICONDATAW g_nid;
static UINT g_taskbar_created;
static int g_running;

/* ---- registry helpers --------------------------------------------------- */

static int reg_get_dword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD *out)
{
    DWORD size = sizeof(*out);
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, NULL, out, &size) == ERROR_SUCCESS;
}

static int reg_get_sz(HKEY root, const wchar_t *key, const wchar_t *name, wchar_t *out, DWORD cap)
{
    DWORD size = cap * sizeof(wchar_t);
    return RegGetValueW(root, key, name, RRF_RT_REG_SZ, NULL, out, &size) == ERROR_SUCCESS;
}

static void reg_set_dword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD v)
{
    RegSetKeyValueW(root, key, name, REG_DWORD, &v, sizeof(v));
}

static void reg_set_sz(HKEY root, const wchar_t *key, const wchar_t *name, const wchar_t *v)
{
    RegSetKeyValueW(root, key, name, REG_SZ, v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

static void reg_delete_value(HKEY root, const wchar_t *key, const wchar_t *name)
{
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, name);
        RegCloseKey(k);
    }
}

/* ---- system proxy ------------------------------------------------------- */

static void notify_inet_change(void)
{
    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

static int proxy_applied(void)
{
    DWORD v = 0;
    return reg_get_dword(HKEY_CURRENT_USER, BACKUP_KEY, L"Applied", &v) && v == 1;
}

static void proxy_apply(void)
{
    if (!proxy_applied()) {
        /* save the user's settings before touching anything */
        DWORD enable = 0;
        wchar_t server[1024], override[2048];
        reg_get_dword(HKEY_CURRENT_USER, INET_KEY, L"ProxyEnable", &enable);
        reg_set_dword(HKEY_CURRENT_USER, BACKUP_KEY, L"ProxyEnable", enable);
        if (reg_get_sz(HKEY_CURRENT_USER, INET_KEY, L"ProxyServer", server, 1024))
            reg_set_sz(HKEY_CURRENT_USER, BACKUP_KEY, L"ProxyServer", server);
        if (reg_get_sz(HKEY_CURRENT_USER, INET_KEY, L"ProxyOverride", override, 2048))
            reg_set_sz(HKEY_CURRENT_USER, BACKUP_KEY, L"ProxyOverride", override);
        reg_set_dword(HKEY_CURRENT_USER, BACKUP_KEY, L"Applied", 1);
    }
    wchar_t server[64];
    swprintf(server, 64, L"127.0.0.1:%d", PROXY_PORT);
    reg_set_dword(HKEY_CURRENT_USER, INET_KEY, L"ProxyEnable", 1);
    reg_set_sz(HKEY_CURRENT_USER, INET_KEY, L"ProxyServer", server);
    reg_set_sz(HKEY_CURRENT_USER, INET_KEY, L"ProxyOverride", L"<local>;localhost;127.*;10.*;192.168.*");
    notify_inet_change();
}

static void proxy_restore(void)
{
    if (!proxy_applied())
        return;
    DWORD enable = 0;
    wchar_t server[1024], override[2048];
    reg_get_dword(HKEY_CURRENT_USER, BACKUP_KEY, L"ProxyEnable", &enable);
    reg_set_dword(HKEY_CURRENT_USER, INET_KEY, L"ProxyEnable", enable);
    if (reg_get_sz(HKEY_CURRENT_USER, BACKUP_KEY, L"ProxyServer", server, 1024))
        reg_set_sz(HKEY_CURRENT_USER, INET_KEY, L"ProxyServer", server);
    else
        reg_delete_value(HKEY_CURRENT_USER, INET_KEY, L"ProxyServer");
    if (reg_get_sz(HKEY_CURRENT_USER, BACKUP_KEY, L"ProxyOverride", override, 2048))
        reg_set_sz(HKEY_CURRENT_USER, INET_KEY, L"ProxyOverride", override);
    else
        reg_delete_value(HKEY_CURRENT_USER, INET_KEY, L"ProxyOverride");
    /* a backup that pointed at Kite itself means the proxy was really off */
    wchar_t ours[64];
    swprintf(ours, 64, L"127.0.0.1:%d", PROXY_PORT);
    if (reg_get_sz(HKEY_CURRENT_USER, INET_KEY, L"ProxyServer", server, 1024) && wcscmp(server, ours) == 0)
        reg_set_dword(HKEY_CURRENT_USER, INET_KEY, L"ProxyEnable", 0);
    RegDeleteKeyW(HKEY_CURRENT_USER, BACKUP_KEY);
    notify_inet_change();
}

/* ---- autostart ---------------------------------------------------------- */

static int autostart_enabled(void)
{
    wchar_t v[MAX_PATH * 2];
    return reg_get_sz(HKEY_CURRENT_USER, RUN_KEY, L"Kite", v, MAX_PATH * 2);
}

static void autostart_set(int on)
{
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --autostart", exe);
        reg_set_sz(HKEY_CURRENT_USER, RUN_KEY, L"Kite", cmd);
    } else {
        reg_delete_value(HKEY_CURRENT_USER, RUN_KEY, L"Kite");
    }
}

/* ---- hosts file (Discord app mode) -------------------------------------- */

static void hosts_path(wchar_t *out, size_t cap)
{
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(out, cap, L"%ls\\drivers\\etc\\hosts", sys);
}

static char *read_file(const wchar_t *path, size_t *len)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *len = (size_t)n;
    }
    return buf;
}

static int hosts_installed(void)
{
    wchar_t path[MAX_PATH];
    hosts_path(path, MAX_PATH);
    size_t len;
    char *buf = read_file(path, &len);
    int found = buf && strstr(buf, HOSTS_BEGIN) != NULL;
    free(buf);
    return found;
}

/* Runs elevated. Rewrites the hosts file with or without Kite's block. */
static int hosts_write(int install)
{
    wchar_t path[MAX_PATH];
    hosts_path(path, MAX_PATH);
    size_t len = 0;
    char *old = read_file(path, &len);
    if (!old)
        return 1;

    size_t cap = len + 8192;
    char *out = malloc(cap);
    if (!out) {
        free(old);
        return 1;
    }
    size_t n = 0;
    int inside = 0;
    for (char *line = old; line && *line;) {
        char *nl = strchr(line, '\n');
        size_t l = nl ? (size_t)(nl - line + 1) : strlen(line);
        if (strncmp(line, HOSTS_BEGIN, strlen(HOSTS_BEGIN)) == 0)
            inside = 1;
        else if (strncmp(line, HOSTS_END, strlen(HOSTS_END)) == 0)
            inside = 0;
        else if (!inside) {
            memcpy(out + n, line, l);
            n += l;
        }
        line = nl ? nl + 1 : NULL;
    }
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        n--;
    n += (size_t)snprintf(out + n, cap - n, "\r\n");
    if (install) {
        n += (size_t)snprintf(out + n, cap - n, "%s\r\n", HOSTS_BEGIN);
        for (int i = 0; discord_domains[i]; i++)
            n += (size_t)snprintf(out + n, cap - n, "127.0.0.1 %s\r\n::1 %s\r\n",
                                  discord_domains[i], discord_domains[i]);
        n += (size_t)snprintf(out + n, cap - n, "%s\r\n", HOSTS_END);
    }
    free(old);

    FILE *f = _wfopen(path, L"wb");
    if (!f) {
        free(out);
        return 1;
    }
    int ok = fwrite(out, 1, n, f) == n;
    fclose(f);
    free(out);

    /* flush the DNS client cache so the change applies immediately */
    HMODULE dns = LoadLibraryW(L"dnsapi.dll");
    if (dns) {
        BOOL (WINAPI *flush)(void) = (BOOL (WINAPI *)(void))(void *)GetProcAddress(dns, "DnsFlushResolverCache");
        if (flush)
            flush();
        FreeLibrary(dns);
    }
    return ok ? 0 : 1;
}

/* Starts an elevated copy of this exe (UAC prompt) and waits for it. */
static int hosts_elevated(int install)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = install ? L"--hosts-install" : L"--hosts-remove";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
        return 1;
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}


/* ---- tray icon ---------------------------------------------------------- */

static HICON load_icon(int id, int size)
{
    return (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, 0);
}

static void tray_update(void)
{
    HICON old = g_nid.hIcon;
    HICON icon = NULL;
    LoadIconMetric(GetModuleHandleW(NULL), MAKEINTRESOURCEW(g_running ? IDI_KITE_ON : IDI_KITE_OFF),
                   LIM_SMALL, &icon);
    g_nid.hIcon = icon;
    wcscpy(g_nid.szTip, g_running ? L"Kite – Açık" : L"Kite – Kapalı");
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (old)
        DestroyIcon(old);
}

static void tray_add(void)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    wcscpy(g_nid.szTip, L"Kite");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    tray_update();
}

static void tray_notify(const wchar_t *title, const wchar_t *text)
{
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title, 63);
    wcsncpy(g_nid.szInfo, text, 255);
    g_nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* ---- engine ------------------------------------------------------------- */

static void engine_start(void)
{
    if (g_running)
        return;
    kite_config cfg;
    kite_config_default(&cfg);
    cfg.listen_port = PROXY_PORT;
    cfg.sni_port = SNI_PORT;
    if (kite_start(&cfg) != 0) {
        tray_notify(L"Kite başlatılamadı", L"8899 portu başka bir program tarafından kullanılıyor olabilir.");
        return;
    }
    proxy_apply();
    g_running = 1;
    tray_update();
}

static void engine_stop(void)
{
    /* restore first so the system never points at a dead proxy */
    proxy_restore();
    if (g_running)
        kite_stop();
    g_running = 0;
    tray_update();
}

/* ---- flyout panel ------------------------------------------------------- */

enum { HIT_NONE, HIT_MAIN, HIT_AUTOSTART, HIT_DISCORD, HIT_QUIT };

typedef struct {
    ARGB bg, card, hover, text, sub, line, track_off, knob_off, accent;
    COLORREF ctext, csub;
} Theme;

static Theme T;
static int g_dpi = 96;
static int g_hover;
static int g_autostart, g_discord;   /* cached, refreshed on open */
static DWORD g_hidden_at;
static HFONT f_title, f_body, f_caption, f_big;
static RECT hit_rects[5];

#define PANEL_W 360
#define PANEL_H 330
#define S(x) MulDiv((x), g_dpi, 96)

static int system_dark(void)
{
    DWORD light = 1;
    reg_get_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                  L"AppsUseLightTheme", &light);
    return light == 0;
}

static void load_theme(void)
{
    if (system_dark()) {
        T = (Theme){ 0xFF202020, 0xFF2B2B2B, 0xFF323232, 0xFFFFFFFF, 0xFFA8A8A8, 0xFF3A3A3A,
                     0xFF3F3F3F, 0xFFCFCFCF, 0xFF22C55E, RGB(255, 255, 255), RGB(168, 168, 168) };
    } else {
        T = (Theme){ 0xFFF3F3F3, 0xFFFFFFFF, 0xFFEAEAEA, 0xFF1A1A1A, 0xFF5F5F5F, 0xFFE0E0E0,
                     0xFFFFFFFF, 0xFF5F5F5F, 0xFF16A34A, RGB(26, 26, 26), RGB(95, 95, 95) };
    }
}

static HFONT make_font(int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void load_fonts(void)
{
    HFONT *all[] = { &f_title, &f_body, &f_caption, &f_big };
    for (int i = 0; i < 4; i++)
        if (*all[i])
            DeleteObject(*all[i]);
    f_title = make_font(18, FW_SEMIBOLD);
    f_body = make_font(14, FW_NORMAL);
    f_caption = make_font(12, FW_NORMAL);
    f_big = make_font(24, FW_SEMIBOLD);
}

static void path_round_rect(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    REAL d = r * 2;
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
}

static void fill_round(GpGraphics *g, ARGB color, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    GpPath *p;
    GpSolidFill *b;
    GdipCreatePath(FillModeAlternate, &p);
    path_round_rect(p, x, y, w, h, r);
    GdipCreateSolidFill(color, &b);
    GdipFillPath(g, (GpBrush *)b, p);
    GdipDeleteBrush((GpBrush *)b);
    GdipDeletePath(p);
}

static void stroke_round(GpGraphics *g, ARGB color, REAL x, REAL y, REAL w, REAL h, REAL r, REAL width)
{
    GpPath *p;
    GpPen *pen;
    GdipCreatePath(FillModeAlternate, &p);
    path_round_rect(p, x, y, w, h, r);
    GdipCreatePen1(color, width, UnitPixel, &pen);
    GdipDrawPath(g, pen, p);
    GdipDeletePen(pen);
    GdipDeletePath(p);
}

static void fill_circle(GpGraphics *g, ARGB color, REAL cx, REAL cy, REAL r)
{
    GpSolidFill *b;
    GdipCreateSolidFill(color, &b);
    GdipFillEllipse(g, (GpBrush *)b, cx - r, cy - r, r * 2, r * 2);
    GdipDeleteBrush((GpBrush *)b);
}

/* Windows 11 style toggle switch; (x, y) is the top-left corner. */
static void draw_switch(GpGraphics *g, int x, int y, int w, int h, int on)
{
    REAL r = (REAL)h / 2;
    if (on) {
        fill_round(g, T.accent, (REAL)x, (REAL)y, (REAL)w, (REAL)h, r);
        fill_circle(g, 0xFFFFFFFF, (REAL)(x + w) - r, (REAL)y + r, r * 0.62f);
    } else {
        fill_round(g, T.track_off, (REAL)x, (REAL)y, (REAL)w, (REAL)h, r);
        stroke_round(g, T.knob_off, (REAL)x + 0.5f, (REAL)y + 0.5f, (REAL)w - 1, (REAL)h - 1, r - 0.5f, (REAL)S(1));
        fill_circle(g, T.knob_off, (REAL)x + r, (REAL)y + r, r * 0.5f);
    }
}

static void text_at(HDC dc, HFONT font, COLORREF color, int x, int y, int w, int h, const wchar_t *s, UINT fmt)
{
    RECT r = { x, y, x + w, y + h };
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

static void fmt_bytes(uint64_t n, wchar_t *out, size_t cap)
{
    const wchar_t *units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)n;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    if (u == 0)
        swprintf(out, cap, L"%llu B", (unsigned long long)n);
    else
        swprintf(out, cap, L"%.1f %ls", v, units[u]);
    for (wchar_t *p = out; *p; p++)
        if (*p == L'.')
            *p = L',';   /* Turkish decimal separator */
}

static void set_hit(int id, int x, int y, int w, int h)
{
    hit_rects[id] = (RECT){ S(x), S(y), S(x + w), S(y + h) };
}

static void paint_panel(HDC target)
{
    int W = S(PANEL_W), H = S(PANEL_H);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ oldbmp = SelectObject(dc, bmp);

    kite_stats st;
    kite_get_stats(&st);

    /* --- shapes (GDI+) --- */
    GpGraphics *g;
    GdipCreateFromHDC(dc, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GpSolidFill *bg;
    GdipCreateSolidFill(T.bg, &bg);
    GdipFillRectangleI(g, (GpBrush *)bg, 0, 0, W, H);
    GdipDeleteBrush((GpBrush *)bg);

    /* header */
    set_hit(HIT_MAIN, 8, 8, PANEL_W - 16, 60);
    if (g_hover == HIT_MAIN)
        fill_round(g, T.hover, (REAL)S(8), (REAL)S(8), (REAL)S(PANEL_W - 16), (REAL)S(60), (REAL)S(6));
    draw_switch(g, S(PANEL_W - 20 - 44), S(28), S(44), S(22), g_running);

    /* stats card */
    fill_round(g, T.card, (REAL)S(16), (REAL)S(80), (REAL)S(PANEL_W - 32), (REAL)S(80), (REAL)S(8));
    stroke_round(g, T.line, (REAL)S(16) + 0.5f, (REAL)S(80) + 0.5f, (REAL)S(PANEL_W - 32) - 1, (REAL)S(80) - 1,
                 (REAL)S(8), 1.0f);
    GpPen *pen;
    GdipCreatePen1(T.line, 1.0f, UnitPixel, &pen);
    GdipDrawLineI(g, pen, S(PANEL_W / 2), S(94), S(PANEL_W / 2), S(146));

    /* option rows */
    set_hit(HIT_AUTOSTART, 8, 172, PANEL_W - 16, 44);
    set_hit(HIT_DISCORD, 8, 218, PANEL_W - 16, 56);
    if (g_hover == HIT_AUTOSTART)
        fill_round(g, T.hover, (REAL)S(8), (REAL)S(172), (REAL)S(PANEL_W - 16), (REAL)S(44), (REAL)S(6));
    if (g_hover == HIT_DISCORD)
        fill_round(g, T.hover, (REAL)S(8), (REAL)S(218), (REAL)S(PANEL_W - 16), (REAL)S(56), (REAL)S(6));
    draw_switch(g, S(PANEL_W - 20 - 40), S(184), S(40), S(20), g_autostart);
    draw_switch(g, S(PANEL_W - 20 - 40), S(236), S(40), S(20), g_discord);

    /* footer */
    GdipDrawLineI(g, pen, 0, S(282), W, S(282));
    GdipDeletePen(pen);
    set_hit(HIT_QUIT, PANEL_W - 16 - 72, 290, 72, 30);
    fill_round(g, g_hover == HIT_QUIT ? T.hover : T.card, (REAL)S(PANEL_W - 16 - 72), (REAL)S(290),
               (REAL)S(72), (REAL)S(30), (REAL)S(5));
    stroke_round(g, T.line, (REAL)S(PANEL_W - 16 - 72) + 0.5f, (REAL)S(290) + 0.5f, (REAL)S(72) - 1,
                 (REAL)S(30) - 1, (REAL)S(5), 1.0f);
    GdipDeleteGraphics(g);

    /* --- icon + text (GDI) --- */
    HICON icon = load_icon(g_running ? IDI_KITE_ON : IDI_KITE_OFF, S(40));
    DrawIconEx(dc, S(20), S(18), icon, S(40), S(40), 0, NULL, DI_NORMAL);
    DestroyIcon(icon);

    SetBkMode(dc, TRANSPARENT);
    text_at(dc, f_title, T.ctext, S(72), S(16), S(200), S(24), L"Kite", DT_LEFT | DT_VCENTER);
    text_at(dc, f_caption, g_running ? RGB(34, 197, 94) : T.csub, S(72), S(40), S(200), S(20),
            g_running ? L"Açık · engeller aşılıyor" : L"Kapalı · tıkla ve aç", DT_LEFT | DT_VCENTER);

    wchar_t num[64], bytes[64];
    swprintf(num, 64, L"%llu", (unsigned long long)st.desynced);
    fmt_bytes(st.bytes_down, bytes, 64);
    text_at(dc, f_caption, T.csub, S(32), S(92), S(140), S(18), L"Korunan bağlantı", DT_LEFT | DT_VCENTER);
    text_at(dc, f_big, T.ctext, S(32), S(112), S(140), S(34), num, DT_LEFT | DT_VCENTER);
    text_at(dc, f_caption, T.csub, S(PANEL_W / 2 + 16), S(92), S(140), S(18), L"İndirilen veri", DT_LEFT | DT_VCENTER);
    text_at(dc, f_big, T.ctext, S(PANEL_W / 2 + 16), S(112), S(140), S(34), bytes, DT_LEFT | DT_VCENTER);

    text_at(dc, f_body, T.ctext, S(20), S(172), S(260), S(44), L"Windows açılınca başlat", DT_LEFT | DT_VCENTER);
    text_at(dc, f_body, T.ctext, S(20), S(224), S(260), S(22), L"Discord uygulaması", DT_LEFT | DT_VCENTER);
    text_at(dc, f_caption, T.csub, S(20), S(246), S(260), S(18), L"Masaüstü uygulaması için · yönetici izni ister",
            DT_LEFT | DT_VCENTER);

    wchar_t ver[48];
    swprintf(ver, 48, L"Kite %hs", kite_version());
    text_at(dc, f_caption, T.csub, S(20), S(290), S(160), S(30), ver, DT_LEFT | DT_VCENTER);
    text_at(dc, f_body, T.ctext, S(PANEL_W - 16 - 72), S(290), S(72), S(30), L"Çıkış", DT_CENTER | DT_VCENTER);

    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static int hit_test(int x, int y)
{
    POINT pt = { x, y };
    for (int i = HIT_MAIN; i <= HIT_QUIT; i++)
        if (PtInRect(&hit_rects[i], pt))
            return i;
    return HIT_NONE;
}

/* Opens the panel above the tray, in the corner of the work area nearest the cursor. */
static void panel_show(void)
{
    g_autostart = autostart_enabled();
    g_discord = hosts_installed();
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    UINT dpi = 96, dpiy;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        HRESULT (WINAPI *get)(HMONITOR, int, UINT *, UINT *) =
            (HRESULT (WINAPI *)(HMONITOR, int, UINT *, UINT *))(void *)GetProcAddress(shcore, "GetDpiForMonitor");
        if (get)
            get(mon, 0, &dpi, &dpiy);
        FreeLibrary(shcore);
    }
    if ((int)dpi != g_dpi) {
        g_dpi = (int)dpi;
        load_fonts();
    }

    int W = S(PANEL_W), H = S(PANEL_H), m = S(12);
    RECT wa = mi.rcWork;
    int x = cur.x > (wa.left + wa.right) / 2 ? wa.right - W - m : wa.left + m;
    int y = cur.y > (wa.top + wa.bottom) / 2 ? wa.bottom - H - m : wa.top + m;
    g_hover = HIT_NONE;
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, W, H, SWP_SHOWWINDOW);
    SetForegroundWindow(g_panel);
    SetTimer(g_panel, 1, 1000, NULL);
    InvalidateRect(g_panel, NULL, FALSE);
}

static void panel_hide(void)
{
    KillTimer(g_panel, 1);
    ShowWindow(g_panel, SW_HIDE);
    g_hidden_at = GetTickCount();
}

static void panel_toggle(void)
{
    if (IsWindowVisible(g_panel))
        panel_hide();
    /* a tray click first deactivates (hides) the panel; don't reopen it */
    else if (GetTickCount() - g_hidden_at > 300)
        panel_show();
}

static void panel_click(int hit)
{
    switch (hit) {
    case HIT_MAIN:
        g_running ? engine_stop() : engine_start();
        break;
    case HIT_AUTOSTART:
        autostart_set(!g_autostart);
        g_autostart = autostart_enabled();
        break;
    case HIT_DISCORD: {
        int on = !g_discord;
        panel_hide(); /* the UAC prompt takes focus anyway */
        if (hosts_elevated(on) != 0)
            tray_notify(L"Discord uygulaması", L"Hosts dosyası değiştirilemedi (izin verilmedi).");
        else if (on)
            tray_notify(L"Discord uygulaması hazır", L"Discord'u tamamen kapatıp yeniden aç.");
        g_discord = hosts_installed();
        return;
    }
    case HIT_QUIT:
        DestroyWindow(g_hwnd);
        return;
    }
    InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint_panel(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int hit = hit_test((short)LOWORD(lp), (short)HIWORD(lp));
        if (hit != g_hover) {
            g_hover = hit;
            InvalidateRect(h, NULL, FALSE);
        }
        SetCursor(LoadCursor(NULL, hit ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = HIT_NONE;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
            return TRUE; /* set in WM_MOUSEMOVE */
        break;
    case WM_LBUTTONUP:
        panel_click(hit_test((short)LOWORD(lp), (short)HIWORD(lp)));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            panel_hide();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            panel_hide();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const wchar_t *)lp, L"ImmersiveColorSet") == 0) {
            load_theme();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void panel_create(HINSTANCE inst)
{
    WNDCLASSW wc = { 0 };
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"KitePanel";
    RegisterClassW(&wc);
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Kite", WS_POPUP,
                              0, 0, S(PANEL_W), S(PANEL_H), NULL, NULL, inst, NULL);
    /* Windows 11: rounded corners and a subtle border (ignored on Windows 10) */
    DWORD corner = 2; /* DWMWCP_ROUND */
    DwmSetWindowAttribute(g_panel, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
}

/* ---- message window ----------------------------------------------------- */

static void show_context_menu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Kite'ı aç");
    AppendMenuW(m, MF_STRING | (g_running ? MF_CHECKED : 0), ID_TOGGLE, L"Engelleri aş");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Çıkış");
    SetMenuDefaultItem(m, ID_OPEN, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created) {
        tray_add(); /* Explorer restarted */
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP)
            panel_toggle();
        else if (LOWORD(lp) == WM_RBUTTONUP)
            show_context_menu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OPEN: panel_show(); break;
        case ID_TOGGLE: g_running ? engine_stop() : engine_start(); break;
        case ID_QUIT: DestroyWindow(h); break;
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp)
            engine_stop();
        return 0;
    case WM_DESTROY:
        engine_stop();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    (void)show;
    if (wcsstr(cmd, L"--hosts-install"))
        return hosts_write(1);
    if (wcsstr(cmd, L"--hosts-remove"))
        return hosts_write(0);

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\KiteTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* already running: ask that instance to open its panel */
        HWND other = FindWindowW(L"KiteTrayWindow", NULL);
        if (other)
            PostMessageW(other, WM_COMMAND, ID_OPEN, 0);
        return 0;
    }

    /* repair settings left behind by a crash or forced kill */
    proxy_restore();

    ULONG_PTR gdip_token;
    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    GdiplusStartup(&gdip_token, &gsi, NULL);
    load_theme();
    load_fonts();

    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"KiteTrayWindow";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Kite", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    panel_create(inst);

    tray_add();
    engine_start();
    if (g_running && !wcsstr(cmd, L"--autostart"))
        tray_notify(L"Kite çalışıyor", L"Engelli siteler artık açılıyor. Ayarlar için sağ alttaki simgeye tıkla.");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(gdip_token);
    ReleaseMutex(mutex);
    return 0;
}
