/*
 * Kite for Windows: system tray app around kite-core.
 *
 * - runs the engine (HTTP/SOCKS5 proxy on 127.0.0.1:8899, TLS on 127.0.0.1:443)
 * - points the per-user WinINet proxy at it (no admin needed) and restores the
 *   previous settings on exit, shutdown, or the next launch after a crash
 * - app support for programs that ignore the system proxy: hosts-file entries
 *   written by an elevated copy of this exe (--hosts-install / --hosts-remove),
 *   asked for once at launch
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
#define WM_KITE_UPDATE (WM_APP + 2)
#define UPDATE_TIMER 2
#define CHECK_RESULT_TIMER 3
#define ID_OPEN 1000
#define ID_TOGGLE 1001
#define ID_QUIT 1004
#define ID_CHECK 1005
#define IDI_KITE_ON 1
#define IDI_KITE_OFF 2

static const wchar_t *INET_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
static const wchar_t *BACKUP_KEY = L"Software\\Kite\\ProxyBackup";
static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char *HOSTS_BEGIN = "# >>> Kite DPI >>>";
static const char *HOSTS_END = "# <<< Kite DPI <<<";
/* markers written by 0.1.x, still recognized so they get replaced */
static const char *HOSTS_LEGACY_BEGIN = "# >>> Kite (Discord) >>>";
static const char *HOSTS_LEGACY_END = "# <<< Kite (Discord) <<<";

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

/* ---- hosts file (app support) ------------------------------------------- */

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
        if (strncmp(line, HOSTS_BEGIN, strlen(HOSTS_BEGIN)) == 0 ||
            strncmp(line, HOSTS_LEGACY_BEGIN, strlen(HOSTS_LEGACY_BEGIN)) == 0)
            inside = 1;
        else if (strncmp(line, HOSTS_END, strlen(HOSTS_END)) == 0 ||
                 strncmp(line, HOSTS_LEGACY_END, strlen(HOSTS_LEGACY_END)) == 0)
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
    wcscpy(g_nid.szTip, g_running ? L"Kite DPI – Açık" : L"Kite DPI – Kapalı");
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
    wcscpy(g_nid.szTip, L"Kite DPI");
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
        tray_notify(L"Kite DPI başlatılamadı", L"8899 portu başka bir program tarafından kullanılıyor olabilir.");
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

/* ---- updates ------------------------------------------------------------
 * Checks GitHub releases at launch and every 6 hours. Installing renames the
 * running exe (Windows allows that), moves the new one into its place, starts
 * it with --after-update and quits; the new instance waits for our mutex. */

enum { UPD_NONE, UPD_AVAILABLE, UPD_INSTALLING, UPD_FAILED };
/* results of a check the user started from the panel */
enum { CHECK_IDLE, CHECK_RUNNING, CHECK_UP_TO_DATE, CHECK_FAILED };
#define UPD_MSG_UP_TO_DATE 10
#define UPD_MSG_CHECK_FAILED 11
static int g_upd_state;
static int g_check_state;
static wchar_t g_upd_version[32];
static char g_upd_url[512];

static int version_newer(const char *a, const char *b)
{
    int x[3] = { 0 }, y[3] = { 0 };
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i])
            return x[i] > y[i];
    return 0;
}

/* Downloads url into a malloc'd buffer (or a file when path is set). */
static char *http_get(const char *url, const wchar_t *path, size_t *len)
{
    HINTERNET net = InternetOpenW(L"Kite DPI", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!net)
        return NULL;
    HINTERNET req = InternetOpenUrlA(net, url, "Accept: application/vnd.github+json\r\n", (DWORD)-1,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    char *buf = NULL;
    size_t n = 0, cap = 0;
    FILE *f = path ? _wfopen(path, L"wb") : NULL;
    DWORD status = 0, slen = sizeof(status);
    if (req && HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &slen, NULL) &&
        status == 200 && (!path || f)) {
        char chunk[16384];
        DWORD got;
        while (InternetReadFile(req, chunk, sizeof(chunk), &got) && got > 0) {
            if (f) {
                fwrite(chunk, 1, got, f);
            } else {
                if (n + got + 1 > cap) {
                    cap = (n + got + 1) * 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) break;
                    buf = nb;
                }
                memcpy(buf + n, chunk, got);
            }
            n += got;
        }
        if (buf)
            buf[n] = 0;
        if (f)
            buf = (char *)1; /* success marker for file downloads */
    }
    if (f)
        fclose(f);
    if (req)
        InternetCloseHandle(req);
    InternetCloseHandle(net);
    if (len)
        *len = n;
    return buf;
}

/* Copies the JSON string value that follows key (e.g. "tag_name":"v0.1.4"). */
static int json_string_after(const char *json, const char *from, const char *key, char *out, size_t cap)
{
    const char *p = strstr(from ? from : json, key);
    if (!p || !(p = strchr(p + strlen(key), '"')))
        return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap)
        out[i++] = *p++;
    out[i] = 0;
    return i > 0;
}

static DWORD WINAPI update_check_thread(void *arg)
{
    int manual = arg != NULL;
    char *json = http_get("https://api.github.com/repos/bkrdmrcioglu/Kite/releases/latest", NULL, NULL);
    if (!json) {
        if (manual)
            PostMessageW(g_hwnd, WM_KITE_UPDATE, UPD_MSG_CHECK_FAILED, 0);
        return 0;
    }
    int found = 0;
    char tag[32], url[512];
    const char *exe = strstr(json, "-Windows-x64.exe\"");
    if (json_string_after(json, NULL, "\"tag_name\"", tag, sizeof(tag)) && exe) {
        /* walk back to the asset's download URL */
        const char *u = exe;
        while (u > json && strncmp(u, "\"browser_download_url\"", 22) != 0)
            u--;
        const char *v = tag[0] == 'v' ? tag + 1 : tag;
        if (u > json && json_string_after(json, u, "\"browser_download_url\"", url, sizeof(url)) &&
            strstr(url, "-Windows-x64.exe") && version_newer(v, kite_version())) {
            strcpy(g_upd_url, url);
            swprintf(g_upd_version, 32, L"%hs", v);
            PostMessageW(g_hwnd, WM_KITE_UPDATE, UPD_AVAILABLE, 0);
            found = 1;
        }
    }
    if (manual && !found)
        PostMessageW(g_hwnd, WM_KITE_UPDATE, UPD_MSG_UP_TO_DATE, 0);
    free(json);
    return 0;
}

/* manual: started from "Güncellemeleri kontrol et", so report the result */
static void update_check(int manual)
{
    if (g_upd_state == UPD_INSTALLING)
        return;
    HANDLE t = CreateThread(NULL, 0, update_check_thread, manual ? (void *)1 : NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

static DWORD WINAPI update_install_thread(void *arg)
{
    (void)arg;
    wchar_t self[MAX_PATH], old[MAX_PATH + 8], tmp[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(old, MAX_PATH + 8, L"%ls.old", self);
    GetTempPathW(MAX_PATH, tmp);
    wcscat(tmp, L"KiteDPI-update.exe");

    size_t len = 0;
    int ok = http_get(g_upd_url, tmp, &len) != NULL && len > 100 * 1024;
    if (ok) {
        FILE *f = _wfopen(tmp, L"rb");
        char mz[2] = { 0 };
        ok = f && fread(mz, 1, 2, f) == 2 && mz[0] == 'M' && mz[1] == 'Z';
        if (f)
            fclose(f);
    }
    if (ok) {
        DeleteFileW(old);
        ok = MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING);
        if (ok && !MoveFileExW(tmp, self, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            MoveFileExW(old, self, MOVEFILE_REPLACE_EXISTING); /* put the old exe back */
            ok = 0;
        }
    }
    DeleteFileW(tmp);
    PostMessageW(g_hwnd, WM_KITE_UPDATE, ok ? UPD_INSTALLING : UPD_FAILED, 0);
    return 0;
}

static void update_install(void)
{
    if (g_upd_state != UPD_AVAILABLE)
        return;
    g_upd_state = UPD_INSTALLING;
    HANDLE t = CreateThread(NULL, 0, update_install_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

/* Called on the UI thread once the new exe is in place. */
static void update_relaunch(void)
{
    wchar_t self[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --after-update", self);
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DestroyWindow(g_hwnd); /* restores the proxy and exits */
}

/* ---- flyout panel ------------------------------------------------------- */

enum { HIT_NONE, HIT_MAIN, HIT_AUTOSTART, HIT_UPDATE, HIT_CHECK, HIT_QUIT };

typedef struct {
    ARGB bg, card, hover, text, sub, line, track_off, knob_off, accent;
    COLORREF ctext, csub;
    ARGB update_bg;
} Theme;

static Theme T;
static int g_dpi = 96;
static int g_hover;
static int g_autostart;   /* cached, refreshed on open */
static DWORD g_hidden_at;
static HFONT f_title, f_body, f_caption, f_big;
static RECT hit_rects[6];

#define PANEL_W 360
#define PANEL_H 300
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
                     0xFF3F3F3F, 0xFFCFCFCF, 0xFF22C55E, RGB(255, 255, 255), RGB(168, 168, 168), 0xFF1E2A3D };
    } else {
        T = (Theme){ 0xFFF3F3F3, 0xFFFFFFFF, 0xFFEAEAEA, 0xFF1A1A1A, 0xFF5F5F5F, 0xFFE0E0E0,
                     0xFFFFFFFF, 0xFF5F5F5F, 0xFF16A34A, RGB(26, 26, 26), RGB(95, 95, 95), 0xFFE3ECFD };
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

/* Extra height while the "update available" row is shown. */
static int update_row(void) { return g_upd_state == UPD_NONE ? 0 : 52; }

static void paint_panel(HDC target)
{
    int u = update_row();
    int W = S(PANEL_W), H = S(PANEL_H + u);
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

    /* update row */
    hit_rects[HIT_UPDATE] = (RECT){ 0, 0, 0, 0 };
    if (u) {
        fill_round(g, T.update_bg, (REAL)S(16), (REAL)S(168), (REAL)S(PANEL_W - 32), (REAL)S(44), (REAL)S(8));
        if (g_upd_state == UPD_AVAILABLE) {
            set_hit(HIT_UPDATE, PANEL_W - 16 - 8 - 84, 175, 84, 30);
            fill_round(g, g_hover == HIT_UPDATE ? 0xFF2563EB : 0xFF3B82F6, (REAL)S(PANEL_W - 16 - 8 - 84),
                       (REAL)S(175), (REAL)S(84), (REAL)S(30), (REAL)S(5));
        }
    }

    /* option rows */
    set_hit(HIT_AUTOSTART, 8, 172 + u, PANEL_W - 16, 44);
    if (g_hover == HIT_AUTOSTART)
        fill_round(g, T.hover, (REAL)S(8), (REAL)S(172 + u), (REAL)S(PANEL_W - 16), (REAL)S(44), (REAL)S(6));
    draw_switch(g, S(PANEL_W - 20 - 40), S(184 + u), S(40), S(20), g_autostart);

    /* footer */
    GdipDrawLineI(g, pen, 0, S(226 + u), W, S(226 + u));
    GdipDeletePen(pen);
    set_hit(HIT_CHECK, 12, 234 + u, 200, 30);
    if (g_hover == HIT_CHECK && g_check_state == CHECK_IDLE)
        fill_round(g, T.hover, (REAL)S(12), (REAL)S(234 + u), (REAL)S(200), (REAL)S(30), (REAL)S(5));
    set_hit(HIT_QUIT, PANEL_W - 16 - 72, 234 + u, 72, 30);
    fill_round(g, g_hover == HIT_QUIT ? T.hover : T.card, (REAL)S(PANEL_W - 16 - 72), (REAL)S(234 + u),
               (REAL)S(72), (REAL)S(30), (REAL)S(5));
    stroke_round(g, T.line, (REAL)S(PANEL_W - 16 - 72) + 0.5f, (REAL)S(234 + u) + 0.5f, (REAL)S(72) - 1,
                 (REAL)S(30) - 1, (REAL)S(5), 1.0f);
    GdipDeleteGraphics(g);

    /* --- icon + text (GDI) --- */
    HICON icon = load_icon(g_running ? IDI_KITE_ON : IDI_KITE_OFF, S(40));
    DrawIconEx(dc, S(20), S(18), icon, S(40), S(40), 0, NULL, DI_NORMAL);
    DestroyIcon(icon);

    SetBkMode(dc, TRANSPARENT);
    text_at(dc, f_title, T.ctext, S(72), S(16), S(200), S(24), L"Kite DPI", DT_LEFT | DT_VCENTER);
    wchar_t ver[24];
    swprintf(ver, 24, L"v%hs", kite_version());
    text_at(dc, f_caption, T.csub, S(152), S(19), S(80), S(20), ver, DT_LEFT | DT_VCENTER);
    text_at(dc, f_caption, g_running ? RGB(34, 197, 94) : T.csub, S(72), S(40), S(200), S(20),
            g_running ? L"Açık · engeller aşılıyor" : L"Kapalı · tıkla ve aç", DT_LEFT | DT_VCENTER);

    wchar_t num[64], bytes[64];
    swprintf(num, 64, L"%llu", (unsigned long long)st.desynced);
    fmt_bytes(st.bytes_down, bytes, 64);
    text_at(dc, f_caption, T.csub, S(32), S(92), S(140), S(18), L"Korunan bağlantı", DT_LEFT | DT_VCENTER);
    text_at(dc, f_big, T.ctext, S(32), S(112), S(140), S(34), num, DT_LEFT | DT_VCENTER);
    text_at(dc, f_caption, T.csub, S(PANEL_W / 2 + 16), S(92), S(140), S(18), L"İndirilen veri", DT_LEFT | DT_VCENTER);
    text_at(dc, f_big, T.ctext, S(PANEL_W / 2 + 16), S(112), S(140), S(34), bytes, DT_LEFT | DT_VCENTER);

    if (u) {
        wchar_t line[96];
        if (g_upd_state == UPD_AVAILABLE)
            swprintf(line, 96, L"Güncelleme var · %ls", g_upd_version);
        else if (g_upd_state == UPD_INSTALLING)
            wcscpy(line, L"Güncelleme indiriliyor…");
        else
            wcscpy(line, L"Güncelleme kurulamadı");
        text_at(dc, f_body, T.ctext, S(30), S(168), S(220), S(44), line, DT_LEFT | DT_VCENTER);
        if (g_upd_state == UPD_AVAILABLE)
            text_at(dc, f_body, RGB(255, 255, 255), S(PANEL_W - 16 - 8 - 84), S(175), S(84), S(30), L"Güncelle",
                    DT_CENTER | DT_VCENTER);
    }

    text_at(dc, f_body, T.ctext, S(20), S(172 + u), S(260), S(44), L"Windows açılınca başlat", DT_LEFT | DT_VCENTER);

    const wchar_t *check = g_check_state == CHECK_RUNNING    ? L"Kontrol ediliyor…"
                         : g_check_state == CHECK_UP_TO_DATE ? L"✓ Kite DPI güncel"
                         : g_check_state == CHECK_FAILED     ? L"Kontrol edilemedi"
                                                             : L"Güncellemeleri kontrol et";
    COLORREF link = g_check_state == CHECK_IDLE ? RGB(59, 130, 246)
                  : g_check_state == CHECK_UP_TO_DATE ? RGB(34, 197, 94) : T.csub;
    text_at(dc, f_body, link, S(20), S(234 + u), S(190), S(30), check, DT_LEFT | DT_VCENTER);
    text_at(dc, f_body, T.ctext, S(PANEL_W - 16 - 72), S(234 + u), S(72), S(30), L"Çıkış", DT_CENTER | DT_VCENTER);
    text_at(dc, f_caption, T.csub, 0, S(270 + u), W, S(22), L"Bekir Demircioğlu tarafından sevgiyle geliştirildi",
            DT_CENTER | DT_VCENTER);

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

    int W = S(PANEL_W), H = S(PANEL_H + update_row()), m = S(12);
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
    case HIT_UPDATE:
        update_install();
        break;
    case HIT_CHECK:
        if (g_check_state != CHECK_RUNNING) {
            g_check_state = CHECK_RUNNING;
            update_check(1);
        }
        break;
    case HIT_AUTOSTART:
        autostart_set(!g_autostart);
        g_autostart = autostart_enabled();
        break;
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
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Kite DPI", WS_POPUP,
                              0, 0, S(PANEL_W), S(PANEL_H), NULL, NULL, inst, NULL);
    /* Windows 11: rounded corners and a subtle border (ignored on Windows 10) */
    DWORD corner = 2; /* DWMWCP_ROUND */
    DwmSetWindowAttribute(g_panel, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
}

/* ---- message window ----------------------------------------------------- */

static void show_context_menu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Kite DPI'ı aç");
    AppendMenuW(m, MF_STRING | (g_running ? MF_CHECKED : 0), ID_TOGGLE, L"Engelleri aş");
    AppendMenuW(m, MF_STRING, ID_CHECK, L"Güncellemeleri kontrol et");
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
    case WM_KITE_UPDATE:
        if (wp == UPD_MSG_UP_TO_DATE || wp == UPD_MSG_CHECK_FAILED) {
            g_check_state = wp == UPD_MSG_UP_TO_DATE ? CHECK_UP_TO_DATE : CHECK_FAILED;
            SetTimer(h, CHECK_RESULT_TIMER, 6000, NULL);
            if (!IsWindowVisible(g_panel)) {
                wchar_t text[96];
                swprintf(text, 96, L"En son sürümü kullanıyorsun (%hs).", kite_version());
                if (wp == UPD_MSG_UP_TO_DATE)
                    tray_notify(L"Kite DPI güncel", text);
                else
                    tray_notify(L"Güncellemeler kontrol edilemedi", L"İnternet bağlantını kontrol et.");
            }
            InvalidateRect(g_panel, NULL, FALSE);
            return 0;
        }
        g_check_state = CHECK_IDLE;
        if (wp == UPD_AVAILABLE && g_upd_state != UPD_AVAILABLE) {
            g_upd_state = UPD_AVAILABLE;
            wchar_t text[128];
            swprintf(text, 128, L"Kite DPI %ls hazır. Kurmak için sağ alttaki simgeye tıkla.", g_upd_version);
            tray_notify(L"Güncelleme var", text);
        } else if (wp == UPD_INSTALLING) {
            update_relaunch();
            return 0;
        } else if (wp == UPD_FAILED) {
            g_upd_state = UPD_FAILED;
            tray_notify(L"Güncelleme kurulamadı",
                        L"Yeni sürümü github.com/bkrdmrcioglu/Kite adresinden indirebilirsin.");
        }
        if (IsWindowVisible(g_panel))
            panel_show(); /* resize for the update row */
        return 0;
    case WM_TIMER:
        if (wp == UPDATE_TIMER) {
            update_check(0);
        } else if (wp == CHECK_RESULT_TIMER) {
            KillTimer(h, CHECK_RESULT_TIMER);
            g_check_state = CHECK_IDLE;
            InvalidateRect(g_panel, NULL, FALSE);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OPEN: panel_show(); break;
        case ID_CHECK:
            g_check_state = CHECK_RUNNING;
            update_check(1);
            break;
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

    int after_update = wcsstr(cmd, L"--after-update") != NULL;
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\KiteTray");
    /* started by the updater: wait for the old version to exit */
    if (GetLastError() == ERROR_ALREADY_EXISTS && after_update)
        WaitForSingleObject(mutex, 15000);
    else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* already running: ask that instance to open its panel */
        HWND other = FindWindowW(L"KiteTrayWindow", NULL);
        if (other)
            PostMessageW(other, WM_COMMAND, ID_OPEN, 0);
        return 0;
    }

    /* repair settings left behind by a crash or forced kill */
    proxy_restore();

    /* remove the previous exe left by an update */
    wchar_t old_exe[MAX_PATH + 8], self_exe[MAX_PATH];
    GetModuleFileNameW(NULL, self_exe, MAX_PATH);
    swprintf(old_exe, MAX_PATH + 8, L"%ls.old", self_exe);
    DeleteFileW(old_exe);

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
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Kite DPI", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    panel_create(inst);

    tray_add();
    engine_start();
    int autostart = wcsstr(cmd, L"--autostart") != NULL;
    /* Some programs ignore the system proxy. Ask once for the admin rights
     * needed to route them through Kite; ask again next launch if declined,
     * but never at login, when nobody may be looking. */
    if (!autostart && !hosts_installed()) {
        int ok = MessageBoxW(NULL,
            L"Kite DPI'ın tarayıcı dışındaki uygulamalarda da çalışabilmesi için bir kez yönetici izni gerekiyor.\n\n"
            L"İzin verdikten sonra tekrar sorulmaz.",
            L"Kite DPI", MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDOK;
        if (ok && hosts_elevated(1) != 0)
            tray_notify(L"Kite DPI", L"İzin verilmedi. Uygulamalar için destek bir sonraki açılışta tekrar sorulacak.");
    }
    if (after_update) {
        wchar_t text[96];
        swprintf(text, 96, L"Kite DPI %hs sürümüne güncellendi.", kite_version());
        tray_notify(L"Güncelleme tamamlandı", text);
    } else if (g_running && !autostart) {
        tray_notify(L"Kite DPI çalışıyor", L"Engelli siteler artık açılıyor. Ayarlar için sağ alttaki simgeye tıkla.");
    }
    update_check(0);
    SetTimer(g_hwnd, UPDATE_TIMER, 6 * 60 * 60 * 1000, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(gdip_token);
    ReleaseMutex(mutex);
    return 0;
}
