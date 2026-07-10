// ============================================================================
//  Silus PSO - Mod / Skin Loader  (ASI plugin for PSOBB, annz1 / mteth client)
// ----------------------------------------------------------------------------
//  Lets players drop replacement asset files into a "mods" folder and have the
//  game load them INSTEAD of the stock files - without ever touching the real
//  data directory of the install.
//
//  HOW IT WORKS
//  ------------
//  PSOBB resolves every asset it opens to a path and hands it to CreateFileA
//  (GENERIC_READ / OPEN_EXISTING). We hook CreateFileA in the game's import
//  table. For each read of an existing file we compute the file's path
//  relative to the game directory, look for the same relative path under each
//  installed mod folder, and if a match exists we transparently open THAT file
//  instead. If no override exists, the call passes through untouched - so with
//  no mods installed this DLL does absolutely nothing observable.
//
//  This is the same idea Ephinea's launcher uses (intercept the asset open and
//  swap the path), implemented as a standalone, address-free IAT hook so it
//  keeps working across client updates.
//
//  BUILD
//  -----
//  Visual Studio -> new "Dynamic-Link Library (DLL)" project.
//    * Platform MUST be x86 / Win32   (PSOBB is 32-bit; x64 will NOT load).
//    * Configuration: Release, /MT (static runtime) recommended so players
//      don't need a VC++ redist.
//    * Add this file. Build. Rename the output to have a .asi extension and
//      drop it wherever your annz1 ASI loader scans (usually next to psobb.exe
//      or in a "plugins"/"asi" folder).
//
//  No external libraries. Pure Win32 + kernel32.
// ============================================================================

#include <windows.h>

// ---------------------------------------------------------------------------
//  Tunables
// ---------------------------------------------------------------------------
#define MAX_MODS      128           // max mod sub-folders
#define PATHBUF       MAX_PATH      // 260

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------
typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);

static CreateFileA_t  g_origCreateFileA = NULL;
static CreateFileW_t  g_origCreateFileW = NULL;

static char  g_gameDir[PATHBUF] = { 0 };   // dir of psobb.exe, no trailing '\'
static size_t g_gameDirLen = 0;
static char  g_selfDir[PATHBUF] = { 0 };   // dir of this .asi, for ini + log
static char  g_modsRoot[PATHBUF] = { 0 };   // <gameDir>\<mods_dir>

static char  g_mods[MAX_MODS][PATHBUF];    // full paths to each mod folder
static int   g_modCount = 0;

static BOOL  g_log = TRUE;   // write silus_mod_loader.log
static BOOL  g_logRedirects = FALSE;  // also log every hit (verbose)
static BOOL  g_ready = FALSE;  // init succeeded

// ---------------------------------------------------------------------------
//  Tiny dependency-free string helpers (no CRT, safe under loader lock)
// ---------------------------------------------------------------------------
static size_t s_len(const char* s) { size_t n = 0; while (s && s[n]) ++n; return n; }

static void s_cpy(char* d, size_t cap, const char* s) {
    size_t i = 0; if (!cap) return; while (s && s[i] && i < cap - 1) { d[i] = s[i]; ++i; } d[i] = 0;
}
static void s_cat(char* d, size_t cap, const char* s) {
    size_t n = s_len(d); if (n >= cap) return; s_cpy(d + n, cap - n, s);
}
static char s_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// minimal unsigned int -> decimal string (avoids pulling in user32 wsprintf)
static void s_utoa(unsigned v, char* out) {
    char tmp[16]; int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    int i = 0; while (n > 0) out[i++] = tmp[--n]; out[i] = 0;
}

// case-insensitive: does 'str' begin with 'pre' ?
static BOOL ci_starts(const char* str, const char* pre) {
    while (*pre) { if (s_lower(*str) != s_lower(*pre)) return FALSE; ++str; ++pre; }
    return TRUE;
}
// normalize forward slashes to backslashes, in place
static void to_backslash(char* s) { for (; *s; ++s) if (*s == '/') *s = '\\'; }

// ---------------------------------------------------------------------------
//  Logging (uses the REAL CreateFileA directly; append mode)
// ---------------------------------------------------------------------------
static void logline(const char* a, const char* b /*optional*/, const char* c /*optional*/) {
    if (!g_log) return;
    char path[PATHBUF]; s_cpy(path, PATHBUF, g_selfDir);
    s_cat(path, PATHBUF, "\\silus_mod_loader.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD w;
    if (a) { WriteFile(h, a, (DWORD)s_len(a), &w, NULL); }
    if (b) { WriteFile(h, b, (DWORD)s_len(b), &w, NULL); }
    if (c) { WriteFile(h, c, (DWORD)s_len(c), &w, NULL); }
    WriteFile(h, "\r\n", 2, &w, NULL);
    CloseHandle(h);
}

// log a single "requested  ->  override" redirect line
static void log_redirect(const char* req, const char* ov) {
    char buf[600];
    s_cpy(buf, sizeof(buf), req);
    s_cat(buf, sizeof(buf), "  ->  ");
    s_cat(buf, sizeof(buf), ov);
    logline("REDIRECT  ", buf, NULL);
}

// ---------------------------------------------------------------------------
//  Does a regular file exist at 'path' ?
// ---------------------------------------------------------------------------
static BOOL file_exists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------------------
//  Turn a requested filename into a "relative asset key".
//   - absolute path under the game dir  -> the part after the game dir
//   - absolute path NOT under game dir   -> "" (leave it alone; external file)
//   - rooted "\foo"                      -> "foo"
//   - already relative "data\foo"        -> "data\foo"
//  Returns FALSE if there is no usable key (skip redirection).
// ---------------------------------------------------------------------------
static BOOL relative_key(const char* requested, char* out, size_t cap) {
    if (!requested || !requested[0]) return FALSE;

    // Drive-absolute?  e.g.  X:\...
    if (requested[1] == ':') {
        if (!ci_starts(requested, g_gameDir)) return FALSE; // outside install: ignore
        const char* rel = requested + g_gameDirLen;
        while (*rel == '\\' || *rel == '/') ++rel;              // strip separator
        if (!*rel) return FALSE;
        s_cpy(out, cap, rel);
    }
    // Rooted on current drive?  \foo
    else if (requested[0] == '\\' || requested[0] == '/') {
        const char* rel = requested;
        while (*rel == '\\' || *rel == '/') ++rel;
        if (!*rel) return FALSE;
        s_cpy(out, cap, rel);
    }
    // Already relative
    else {
        s_cpy(out, cap, requested);
    }
    to_backslash(out);
    return out[0] != 0;
}

// ---------------------------------------------------------------------------
//  Look for an override.  First mod (in priority order) that has the file wins.
//  On success fills 'out' with the full override path and returns TRUE.
// ---------------------------------------------------------------------------
static BOOL find_override(const char* requested, char* out, size_t cap) {
    if (!g_ready || g_modCount == 0) return FALSE;

    char key[PATHBUF];
    if (!relative_key(requested, key, PATHBUF)) return FALSE;

    for (int i = 0;i < g_modCount;++i) {
        char cand[PATHBUF];
        s_cpy(cand, PATHBUF, g_mods[i]);
        s_cat(cand, PATHBUF, "\\");
        s_cat(cand, PATHBUF, key);
        if (file_exists(cand)) {
            s_cpy(out, cap, cand);
            return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
//  The hooks.  Only redirect reads of existing files (asset loads).
//  Writes / file creation are never touched.
// ---------------------------------------------------------------------------
static BOOL is_asset_read(DWORD access, DWORD disposition) {
    return disposition == OPEN_EXISTING && !(access & GENERIC_WRITE);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES sa, DWORD disp,
    DWORD flags, HANDLE tmpl) {
    if (name && is_asset_read(access, disp)) {
        char ov[PATHBUF];
        if (find_override(name, ov, PATHBUF)) {
            if (g_logRedirects) log_redirect(name, ov);
            return g_origCreateFileA(ov, access, share, sa, disp, flags, tmpl);
        }
    }
    return g_origCreateFileA(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES sa, DWORD disp,
    DWORD flags, HANDLE tmpl) {
    if (name && is_asset_read(access, disp)) {
        // PSOBB asset paths are ASCII; widen->narrow is lossless here.
        char narrow[PATHBUF];
        int n = WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, PATHBUF, NULL, NULL);
        if (n > 0) {
            char ov[PATHBUF];
            if (find_override(narrow, ov, PATHBUF)) {
                WCHAR wov[PATHBUF];
                if (MultiByteToWideChar(CP_ACP, 0, ov, -1, wov, PATHBUF) > 0) {
                    if (g_logRedirects) log_redirect(narrow, ov);
                    return g_origCreateFileW(wov, access, share, sa, disp, flags, tmpl);
                }
            }
        }
    }
    return g_origCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

// ---------------------------------------------------------------------------
//  IAT patching - value-match, so we don't depend on any game address and it
//  works regardless of which import descriptor / binding CreateFile lives in.
// ---------------------------------------------------------------------------
static void patch_module(HMODULE hMod, PROC realA, PROC realW) {
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        IMAGE_THUNK_DATA* th = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; th->u1.Function; ++th) {
            PROC cur = (PROC)(UINT_PTR)th->u1.Function;
            void* repl = NULL;
            if (cur == realA)      repl = (void*)&Hook_CreateFileA;
            else if (cur == realW) repl = (void*)&Hook_CreateFileW;
            if (repl) {
                DWORD old;
                if (VirtualProtect(&th->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    th->u1.Function = (UINT_PTR)repl;
                    VirtualProtect(&th->u1.Function, sizeof(void*), old, &old);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  Mod discovery + load order
// ---------------------------------------------------------------------------
static void add_mod(const char* fullpath) {
    if (g_modCount >= MAX_MODS) return;
    s_cpy(g_mods[g_modCount], PATHBUF, fullpath);
    ++g_modCount;
}

// case-insensitive insertion sort of g_mods by folder basename
static void sort_mods_alpha() {
    for (int i = 1;i < g_modCount;++i) {
        char tmp[PATHBUF]; s_cpy(tmp, PATHBUF, g_mods[i]);
        int j = i - 1;
        while (j >= 0) {
            // compare basenames
            const char* a = g_mods[j]; const char* an = a; for (const char* p = a;*p;++p) if (*p == '\\') an = p + 1;
            const char* b = tmp;       const char* bn = b; for (const char* p = b;*p;++p) if (*p == '\\') bn = p + 1;
            int cmp = 0; const char* x = an; const char* y = bn;
            while (*x && *y) { char cx = s_lower(*x), cy = s_lower(*y); if (cx != cy) { cmp = (cx < cy) ? -1 : 1;break; } ++x;++y; }
            if (cmp == 0) cmp = (*x ? 1 : (*y ? -1 : 0));
            if (cmp <= 0) break;
            s_cpy(g_mods[j + 1], PATHBUF, g_mods[j]);
            --j;
        }
        s_cpy(g_mods[j + 1], PATHBUF, tmp);
    }
}

// Read mods\load_order.txt (if present) and move listed folders to the front,
// in the listed order. First line = highest priority. Unlisted folders keep
// their (alphabetical) order behind the listed ones.
static void apply_load_order() {
    char lo[PATHBUF]; s_cpy(lo, PATHBUF, g_modsRoot);
    s_cat(lo, PATHBUF, "\\load_order.txt");
    HANDLE h = CreateFileA(lo, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[8192]; DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    int front = 0; // next slot to fill with a listed mod
    char* p = buf;
    while (*p) {
        // extract a line
        char name[PATHBUF]; int n = 0;
        while (*p == '\r' || *p == '\n') ++p;
        while (*p && *p != '\r' && *p != '\n' && n < PATHBUF - 1) { name[n++] = *p++; }
        name[n] = 0;
        // trim trailing spaces
        while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t')) name[--n] = 0;
        if (n == 0 || name[0] == ';' || name[0] == '#') continue;

        // find this folder in g_mods[front..] and swap to 'front'
        for (int i = front;i < g_modCount;++i) {
            const char* bn = g_mods[i]; for (const char* q = g_mods[i];*q;++q) if (*q == '\\') bn = q + 1;
            // case-insensitive full match of basename
            const char* x = bn; const char* y = name; BOOL eq = TRUE;
            while (*x && *y) { if (s_lower(*x) != s_lower(*y)) { eq = FALSE;break; } ++x;++y; }
            if (eq && !*x && !*y) {
                if (i != front) {
                    char t[PATHBUF]; s_cpy(t, PATHBUF, g_mods[front]);
                    s_cpy(g_mods[front], PATHBUF, g_mods[i]);
                    s_cpy(g_mods[i], PATHBUF, t);
                }
                ++front; break;
            }
        }
    }
}

static void discover_mods() {
    char search[PATHBUF]; s_cpy(search, PATHBUF, g_modsRoot);
    s_cat(search, PATHBUF, "\\*");

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.' && (fd.cFileName[1] == 0 ||
            (fd.cFileName[1] == '.' && fd.cFileName[2] == 0))) continue;
        char full[PATHBUF]; s_cpy(full, PATHBUF, g_modsRoot);
        s_cat(full, PATHBUF, "\\"); s_cat(full, PATHBUF, fd.cFileName);
        add_mod(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    sort_mods_alpha();
    apply_load_order();
}

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------
static void dir_of(const char* full, char* out, size_t cap) {
    s_cpy(out, cap, full);
    for (int i = (int)s_len(out) - 1;i >= 0;--i) { if (out[i] == '\\' || out[i] == '/') { out[i] = 0; break; } }
}

static void init(HMODULE hSelf) {
    // game dir (from the host exe)
    char exePath[PATHBUF];
    GetModuleFileNameA(NULL, exePath, PATHBUF);
    dir_of(exePath, g_gameDir, PATHBUF);
    g_gameDirLen = s_len(g_gameDir);

    // self dir (for ini + log)
    char selfPath[PATHBUF];
    GetModuleFileNameA(hSelf, selfPath, PATHBUF);
    dir_of(selfPath, g_selfDir, PATHBUF);

    // config: silus_mod_loader.ini next to the .asi
    char ini[PATHBUF]; s_cpy(ini, PATHBUF, g_selfDir);
    s_cat(ini, PATHBUF, "\\silus_mod_loader.ini");
    g_log = GetPrivateProfileIntA("settings", "log", 1, ini) != 0;
    g_logRedirects = GetPrivateProfileIntA("settings", "log_redirects", 0, ini) != 0;
    char modsDir[128];
    GetPrivateProfileStringA("settings", "mods_dir", "mods", modsDir, sizeof(modsDir), ini);

    // mods root = <gameDir>\<mods_dir>
    s_cpy(g_modsRoot, PATHBUF, g_gameDir);
    s_cat(g_modsRoot, PATHBUF, "\\"); s_cat(g_modsRoot, PATHBUF, modsDir);

    discover_mods();

    // install the hooks in the main exe's import table
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_origCreateFileA = (CreateFileA_t)GetProcAddress(k32, "CreateFileA");
    g_origCreateFileW = (CreateFileW_t)GetProcAddress(k32, "CreateFileW");
    if (g_origCreateFileA) {
        patch_module(GetModuleHandleA(NULL),
            (PROC)g_origCreateFileA, (PROC)g_origCreateFileW);
    }

    g_ready = (g_origCreateFileA != NULL);

    if (g_log) {
        logline("---- Silus Mod Loader ----", NULL, NULL);
        logline("game dir : ", g_gameDir, NULL);
        logline("mods root: ", g_modsRoot, NULL);
        char n[16]; s_utoa((unsigned)g_modCount, n);
        logline("mods     : ", n, " (in priority order)");
        for (int i = 0;i < g_modCount;++i) {
            char idx[16]; s_utoa((unsigned)i, idx);
            char pre[24]; s_cpy(pre, sizeof(pre), "  ["); s_cat(pre, sizeof(pre), idx); s_cat(pre, sizeof(pre), "] ");
            logline(pre, g_mods[i], NULL);
        }
        logline(g_ready ? "status   : ACTIVE" : "status   : FAILED (CreateFileA not found)",
            NULL, NULL);
    }
}

// ---------------------------------------------------------------------------
//  Entry points
//
//  patch.dll's built-in ASI loader scans patches/ (and the game root) for .asi
//  files and calls each one's exported  void __stdcall load(void).  We do our
//  setup there - the same point Silus's Hangame ASI uses - rather than in
//  DllMain, to stay off the loader lock during file I/O.
// ---------------------------------------------------------------------------
static HMODULE      g_self = NULL;
static volatile LONG g_initOnce = 0;

static void ensure_init() {
    if (InterlockedCompareExchange(&g_initOnce, 1, 0) != 0) return; // run exactly once
    init(g_self ? g_self : GetModuleHandleA(NULL));
}

// Exported under the plain name "load" (see the /EXPORT pragma below).
extern "C" void __stdcall load(void) {
    ensure_init();
}

// void __stdcall load(void) decorates to _load@0 under MSVC; re-export it under
// the undecorated name the loader resolves via GetProcAddress(h, "load").
// (Same fix Silus used in Hangame; alternative is a .def file.)
#if defined(_MSC_VER)
#pragma comment(linker, "/EXPORT:load=_load@0")
#endif

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        g_self = hinst;   // used by init() for the ini/log location
    }
    return TRUE;
}