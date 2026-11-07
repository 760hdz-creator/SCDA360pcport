// SCDA Recomp installer. Takes the user's own Splinter Cell: Double Agent (X360) ISO,
// extracts its XDVDFS volume into <dir>\game, verifies SC4_Offline.xex is the exact
// binary the recompiled code was generated from, and writes the embedded scda.exe.
// Run again over an install, it offers an update instead: only scda.exe is replaced (no ISO
// needed); game data, settings, saves and shortcuts are left alone.
// Ships no game data.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "resource.h"

// SHA-256 of the SC4_Offline.xex that generated/ was recompiled from.
static const char* kExpectedXexSha256 = "86d32713b57feb6631f0810a9416046beb3a2157084498e61f5b59b669e82a93";
// SHA-256 of the flat image xex_dump builds from it (config/expected_image.txt).
static const char* kExpectedImageSha256 = "01193b2556abc03506ad9c0d40f30b04ce27d027e27a59cf786eeb90b53aea4e";

static const wchar_t kAppName[]    = L"Splinter Cell: Double Agent (Recomp)";
static const wchar_t kFolderName[] = L"SCDA-Recomp";
static const wchar_t kShortcut[]   = L"Splinter Cell Double Agent (Recomp).lnk";
// Per-user record of the install (no administrator rights): InstallLocation, Version.
static const wchar_t kRegKey[]     = L"Software\\SCDARecomp";
#ifndef SCDA_VERSION
#define SCDA_VERSION "dev"
#endif
static const wchar_t kVersion[]    = L"" SCDA_VERSION; // git short hash (build.bat)

constexpr uint32_t SECTOR = 2048;
constexpr uint64_t kXgdBases[] = { 0, 0x18300000, 0xFD90000, 0x2080000 }; // xiso, XGD3, XGD2, XGD1

enum { ID_ISO_EDIT = 100, ID_ISO_BROWSE, ID_DIR_EDIT, ID_DIR_BROWSE, ID_INSTALL, ID_PROGRESS, ID_STATUS,
       ID_SC_DESKTOP, ID_SC_STARTMENU, ID_CREDIT };
constexpr UINT WM_APP_STATUS = WM_APP + 1, WM_APP_PROGRESS = WM_APP + 2, WM_APP_DONE = WM_APP + 3, WM_APP_ASK = WM_APP + 4;

static HWND g_wnd;
static bool g_headless, g_ok, g_updating;
static std::wstring g_error;
static std::atomic<bool> g_cancel;

struct Entry { std::wstring path; uint32_t sector, size; bool dir; };

static void PostStatus(const std::wstring& s)
{
    if (g_headless)
        fwprintf(stdout, L"%ls\n", s.c_str());
    else
        PostMessageW(g_wnd, WM_APP_STATUS, 0, (LPARAM)new std::wstring(s));
}

// ---- XDVDFS (ported from tools/pe_ppc_inspect.py) ----

static bool ReadAt(HANDLE f, uint64_t off, void* buf, DWORD size)
{
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, buf, size, &got, nullptr) && got == size;
}

static bool FindVolume(HANDLE f, uint64_t& base, uint32_t& rootSector, uint32_t& rootSize)
{
    for (uint64_t b : kXgdBases)
    {
        uint8_t vd[28];
        if (ReadAt(f, b + 32ull * SECTOR, vd, sizeof(vd)) && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20))
        {
            base = b;
            memcpy(&rootSector, vd + 20, 4);
            memcpy(&rootSize, vd + 24, 4);
            return true;
        }
    }
    return false;
}

static void WalkDir(HANDLE f, uint64_t base, uint32_t sector, uint32_t size, const std::wstring& prefix, std::vector<Entry>& out, int depth = 0)
{
    if (depth > 32 || size == 0 || size > 64 * 1024 * 1024)
        return;
    std::vector<uint8_t> data(size);
    if (!ReadAt(f, base + uint64_t(sector) * SECTOR, data.data(), size))
        return;

    std::vector<uint32_t> stack{ 0 };
    std::vector<bool> seen(size / 4 + 1);
    while (!stack.empty())
    {
        uint32_t off = stack.back();
        stack.pop_back();
        if (off * 4 + 14 > size || seen[off])
            continue;
        seen[off] = true;
        const uint8_t* e = data.data() + off * 4;
        uint16_t left, right; uint32_t esec, esize;
        memcpy(&left, e, 2); memcpy(&right, e + 2, 2); memcpy(&esec, e + 4, 4); memcpy(&esize, e + 8, 4);
        uint8_t attr = e[12], nlen = e[13];
        if (left == 0xFFFF && right == 0xFFFF && esec == 0xFFFFFFFF)
            continue; // padding
        if (off * 4 + 14 + nlen > size)
            continue;
        std::wstring name;
        for (int i = 0; i < nlen; i++)
            name += wchar_t(e[14 + i]);
        std::wstring path = prefix + name;
        if (attr & 0x10)
        {
            out.push_back({ path, esec, esize, true });
            WalkDir(f, base, esec, esize, path + L"\\", out, depth + 1);
        }
        else
        {
            out.push_back({ path, esec, esize, false });
        }
        for (uint16_t next : { left, right })
            if (next != 0 && next != 0xFFFF)
                stack.push_back(next);
    }
}

// ---- Helpers ----

static std::string Sha256File(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hash;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> buf(1 << 20);
    DWORD got;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
        BCryptHashData(hash, buf.data(), got, 0);
    uint8_t digest[32];
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    std::string hex;
    char tmp[3];
    for (uint8_t b : digest) { snprintf(tmp, 3, "%02x", b); hex += tmp; }
    return hex;
}

static bool WriteResource(int id, const std::wstring& path)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    DWORD size = SizeofResource(nullptr, r);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote;
    bool ok = WriteFile(f, LockResource(g), size, &wrote, nullptr) && wrote == size;
    CloseHandle(f);
    return ok;
}

// Runs the embedded xex_dump: SC4_Offline.xex -> flat image + XEX header (what scda.exe loads).
static bool BuildImage(const std::wstring& game)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring tool = std::wstring(tmp) + L"scda_xex_dump.exe";
    if (!WriteResource(IDR_XEX_DUMP, tool))
        return false;
    std::wstring cmd = L"\"" + tool + L"\" \"" + game + L"\\SC4_Offline.xex\" --out-img \"" + game +
        L"\\SC4_Offline.img\" --out-header \"" + game + L"\\SC4_Offline.xexhdr\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (CreateProcessW(tool.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DeleteFileW(tool.c_str());
    return code == 0 && Sha256File(game + L"\\SC4_Offline.img") == kExpectedImageSha256;
}

static std::wstring KnownDir(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) { out = p; CoTaskMemFree(p); }
    return out;
}

static void MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& workDir)
{
    IShellLinkW* link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(kAppName);
    IPersistFile* file;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))))
    {
        file->Save(lnk.c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

// ---- Existing install ----

static void SaveInstallRecord(const std::wstring& dir)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    auto set = [&](const wchar_t* n, const std::wstring& v)
    { RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t))); };
    set(L"InstallLocation", dir);
    set(L"Version", kVersion);
    RegCloseKey(k);
}

static std::wstring ReadRecord(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// A folder this installer wrote: scda.exe next to game\SC4_Offline.img.
static bool IsInstallDir(const std::wstring& dir)
{
    return !dir.empty() && GetFileAttributesW((dir + L"\\scda.exe").c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW((dir + L"\\game\\SC4_Offline.img").c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The registry record, else the folder our Start menu / Desktop shortcut points at (installs made
// before the record existed). Empty when there is none.
static std::wstring FindInstall()
{
    std::wstring dir = ReadRecord(L"InstallLocation");
    if (IsInstallDir(dir))
        return dir;
    for (REFKNOWNFOLDERID id : { FOLDERID_Programs, FOLDERID_Desktop })
    {
        std::wstring lnk = KnownDir(id);
        if (lnk.empty()) continue;
        lnk += L"\\";
        lnk += kShortcut;
        IShellLinkW* link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
            continue;
        IPersistFile* file;
        wchar_t target[MAX_PATH] = {};
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))))
        {
            if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ)))
                link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH);
            file->Release();
        }
        link->Release();
        dir = target;
        const size_t slash = dir.find_last_of(L'\\');
        if (slash != std::wstring::npos)
            dir.resize(slash);
        if (IsInstallDir(dir))
            return dir;
    }
    return {};
}

// Creates d and any missing parents, recording each folder it made.
static void MakeDirs(const std::wstring& d, std::vector<std::wstring>& made)
{
    if (d.size() <= 3 || GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    size_t slash = d.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        MakeDirs(d.substr(0, slash), made);
    if (CreateDirectoryW(d.c_str(), nullptr))
        made.push_back(d);
}

// ---- Install worker ----

static void Install(std::wstring iso, std::wstring dir, bool scDesktop, bool scStartMenu)
{
    // A failed or cancelled install deletes what it wrote: files, then the folders it created.
    std::vector<std::wstring> wroteFiles, madeDirs;
    auto makeDir = [&](const std::wstring& d) { MakeDirs(d, madeDirs); };
    auto fail = [&](const std::wstring& msg)
    {
        for (auto& p : wroteFiles) DeleteFileW(p.c_str());
        for (auto it = madeDirs.rbegin(); it != madeDirs.rend(); ++it) RemoveDirectoryW(it->c_str());
        g_error = msg;
        if (g_headless) PostStatus(msg);
        else PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    };

    HANDLE f = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return fail(L"Cannot open the ISO.");

    uint64_t base; uint32_t rootSector, rootSize;
    if (!FindVolume(f, base, rootSector, rootSize))
    {
        CloseHandle(f);
        return fail(L"Not an Xbox 360 disc image (no XDVDFS volume).");
    }

    PostStatus(L"Reading disc layout\x2026");
    std::vector<Entry> entries;
    WalkDir(f, base, rootSector, rootSize, L"", entries);

    bool hasXex = false;
    uint64_t total = 0;
    for (auto& e : entries)
    {
        if (!e.dir) total += e.size;
        if (!_wcsicmp(e.path.c_str(), L"SC4_Offline.xex")) hasXex = true;
    }
    if (!hasXex)
    {
        CloseHandle(f);
        return fail(L"SC4_Offline.xex not found: this is not Splinter Cell: Double Agent.");
    }

    std::wstring game = dir + L"\\game";
    makeDir(dir);
    makeDir(game);

    // Verify the executable first so a wrong disc fails fast.
    std::vector<uint8_t> buf(4 << 20);
    uint64_t done = 0;
    auto extract = [&](const Entry& e) -> bool
    {
        std::wstring out = game + L"\\" + e.path;
        HANDLE o = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (o == INVALID_HANDLE_VALUE) return false;
        wroteFiles.push_back(out);
        uint64_t off = base + uint64_t(e.sector) * SECTOR;
        uint32_t left = e.size;
        while (left)
        {
            DWORD n = std::min<uint32_t>(left, (uint32_t)buf.size()), wrote;
            if (g_cancel || !ReadAt(f, off, buf.data(), n) || !WriteFile(o, buf.data(), n, &wrote, nullptr))
            {
                CloseHandle(o);
                return false;
            }
            off += n; left -= n; done += n;
            PostMessageW(g_wnd, WM_APP_PROGRESS, (WPARAM)(total ? done * 950 / total : 0), 0);
        }
        CloseHandle(o);
        return true;
    };
    auto cancelled = [&] { CloseHandle(f); fail(L"Installation cancelled."); };

    for (auto& e : entries)
    {
        if (!_wcsicmp(e.path.c_str(), L"SC4_Offline.xex"))
        {
            PostStatus(L"Verifying SC4_Offline.xex\x2026");
            if (!extract(e)) { if (g_cancel) return cancelled(); CloseHandle(f); return fail(L"Failed writing SC4_Offline.xex."); }
            if (Sha256File(game + L"\\SC4_Offline.xex") != kExpectedXexSha256)
            {
                CloseHandle(f);
                return fail(L"SC4_Offline.xex does not match the supported version (V1, Title 555307D7).");
            }
        }
    }

    for (auto& e : entries)
    {
        if (g_cancel) return cancelled();
        if (e.dir)
        {
            makeDir(game + L"\\" + e.path);
            continue;
        }
        if (!_wcsicmp(e.path.c_str(), L"SC4_Offline.xex"))
            continue;
        PostStatus(L"Extracting " + e.path);
        if (!extract(e)) { if (g_cancel) return cancelled(); CloseHandle(f); return fail(L"Failed extracting " + e.path); }
    }
    CloseHandle(f);

    PostStatus(L"Building SC4_Offline.img\x2026");
    wroteFiles.push_back(game + L"\\SC4_Offline.img");
    wroteFiles.push_back(game + L"\\SC4_Offline.xexhdr");
    if (!BuildImage(game))
        return fail(L"Failed building SC4_Offline.img from SC4_Offline.xex.");
    PostMessageW(g_wnd, WM_APP_PROGRESS, 980, 0);

    PostStatus(L"Installing scda.exe\x2026");
    std::wstring exe = dir + L"\\scda.exe";
    wroteFiles.push_back(exe);
    if (!WriteResource(IDR_SCDA_EXE, exe))
        return fail(L"Failed writing scda.exe.");

    PostStatus(L"Creating shortcuts\x2026");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (scDesktop)
    {
        std::wstring d = KnownDir(FOLDERID_Desktop);
        if (!d.empty()) MakeShortcut(d + L"\\" + kShortcut, exe, dir);
    }
    if (scStartMenu)
    {
        // Per-user Start menu: this installer runs without administrator rights.
        std::wstring s = KnownDir(FOLDERID_Programs);
        if (!s.empty()) MakeShortcut(s + L"\\" + kShortcut, exe, dir);
    }
    CoUninitialize();

    SaveInstallRecord(dir);
    PostMessageW(g_wnd, WM_APP_PROGRESS, 1000, 0);
    g_ok = true;
    PostStatus(L"Done. Installed to " + dir + L"  (developer preview)");
    PostMessageW(g_wnd, WM_APP_DONE, 1, 0);
}

// ---- Update worker ----

// Replaces scda.exe in an existing install with the embedded one. Needs no ISO: the game data
// stay, so the image must be the one this scda.exe was recompiled from.
static void Update(std::wstring dir)
{
    auto fail = [&](const std::wstring& msg)
    {
        g_error = msg;
        if (g_headless) PostStatus(msg);
        else PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    };
    const std::wstring exe = dir + L"\\scda.exe", staged = dir + L"\\scda.exe.new";
    if (!IsInstallDir(dir))
        return fail(L"No installed game was found in " + dir + L".");

    // A running exe cannot be opened for writing.
    HANDLE h = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail(L"The game is running (or scda.exe is in use). Close it and try again.");
    CloseHandle(h);

    PostStatus(L"Checking the installed game data\x2026");
    PostMessageW(g_wnd, WM_APP_PROGRESS, 100, 0);
    if (Sha256File(dir + L"\\game\\SC4_Offline.img") != kExpectedImageSha256)
        return fail(L"The installed game data do not match this version. Choose Full reinstall instead.");

    PostStatus(L"Installing scda.exe\x2026");
    PostMessageW(g_wnd, WM_APP_PROGRESS, 600, 0);
    if (!WriteResource(IDR_SCDA_EXE, staged))
    {
        DeleteFileW(staged.c_str());
        return fail(L"Failed writing scda.exe.");
    }
    if (!MoveFileExW(staged.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(staged.c_str());
        return fail(L"Could not replace scda.exe (is the game running?).");
    }
    SaveInstallRecord(dir);
    PostMessageW(g_wnd, WM_APP_PROGRESS, 1000, 0);
    g_ok = true;
    PostStatus(L"Done. Updated " + dir + L" to version " + kVersion);
    PostMessageW(g_wnd, WM_APP_DONE, 1, 0);
}

// ---- Artwork ----
//
// The window is res/installer_bg.png (IDR_INSTALLER_BG): the art draws the fields and buttons,
// and the real controls sit on top of them. Edits get the field colour; buttons are owner-drawn
// from the art itself (plus a hover/press tint). Below the art a plain strip holds what the art has
// no place for: progress, status, the shortcut boxes and the credit. Everything scales with the art.

static const int kArtW = 1671, kArtH = 941, kStripH = 80; // art pixels
static const COLORREF kFieldColor = RGB(11, 26, 22), kStripColor = RGB(5, 10, 7);
static const COLORREF kTextColor = RGB(226, 238, 228), kDimColor = RGB(140, 160, 145);
static const COLORREF kAccent = RGB(80, 230, 40);

// Interiors of the two drawn fields: painted over with kFieldColor (they hold sample text).
static const RECT kArtFields[] = { { 489, 621, 1068, 660 }, { 489, 716, 1068, 755 } };
static const int  kFieldIds[]  = { ID_ISO_EDIT, ID_DIR_EDIT };
struct ArtRect { int id; RECT r; };
static const ArtRect kArtLayout[] = {
    { ID_ISO_BROWSE, { 1090, 614, 1287, 664 } },
    { ID_DIR_BROWSE, { 1090, 709, 1287, 760 } },
    { ID_INSTALL,    {  415, 776, 1260, 870 } },
    { IDCANCEL,      { 1595,   5, 1665,  70 } }, // the X drawn in the corner
};

static double  g_scale = 1;
static HDC     g_artDC;  // the art at window size, field sample text painted out
static HBITMAP g_artBmp, g_artOld;
static HFONT   g_fieldFont, g_uiFont;
static HBRUSH  g_fieldBrush, g_stripBrush;
static std::thread g_worker;
static bool    g_running;

static int  S(int v) { return (int)lround(v * g_scale); }
static RECT S(const RECT& r) { return { S(r.left), S(r.top), S(r.right), S(r.bottom) }; }

static HBITMAP LoadArt(int w, int h)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_INSTALLER_BG), RT_RCDATA);
    HGLOBAL g = res ? LoadResource(nullptr, res) : nullptr;
    BYTE* data = g ? (BYTE*)LockResource(g) : nullptr;
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || !size) return nullptr;
    IWICImagingFactory* f = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    HBITMAP bmp = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(data, size)) &&
        SUCCEEDED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && FAILED(conv->CopyPixels(nullptr, (UINT)w * 4, (UINT)w * 4 * (UINT)h, (BYTE*)bits)))
        {
            DeleteObject(bmp);
            bmp = nullptr;
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{ conv, scaler, frame, dec, stream, f }) if (u) u->Release();
    return bmp;
}

// A translucent wash over r (hover / press / disabled states of the drawn buttons).
static void Tint(HDC dc, const RECT& r, COLORREF c, BYTE alpha)
{
    HDC src = CreateCompatibleDC(dc);
    HBITMAP px = CreateCompatibleBitmap(dc, 1, 1);
    HGDIOBJ old = SelectObject(src, px);
    SetPixel(src, 0, 0, c);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, src, 0, 0, 1, 1, bf);
    SelectObject(src, old);
    DeleteObject(px);
    DeleteDC(src);
}

// Hover tracking + hand cursor for the drawn buttons (hover flag in GWLP_USERDATA).
static LRESULT CALLBACK ArtButtonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!GetWindowLongPtrW(h, GWLP_USERDATA))
        {
            SetWindowLongPtrW(h, GWLP_USERDATA, 1);
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_SETCURSOR:
        if (IsWindowEnabled(h)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void DrawArtButton(HWND wnd, const DRAWITEMSTRUCT* di)
{
    RECT r = di->rcItem;
    if (g_artDC)
    {
        RECT at;
        GetWindowRect(di->hwndItem, &at);
        MapWindowPoints(nullptr, wnd, (POINT*)&at, 2);
        BitBlt(di->hDC, 0, 0, r.right, r.bottom, g_artDC, at.left, at.top, SRCCOPY);
    }
    else // no art: plain dark buttons with their captions
    {
        FillRect(di->hDC, &r, g_fieldBrush);
        wchar_t text[64];
        GetWindowTextW(di->hwndItem, text, 64);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, kTextColor);
        HGDIOBJ of = SelectObject(di->hDC, g_uiFont);
        DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(di->hDC, of);
    }
    const bool hover = GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA) != 0;
    if (di->itemState & ODS_DISABLED) Tint(di->hDC, r, RGB(0, 0, 0), 130);
    else if (di->itemState & ODS_SELECTED) Tint(di->hDC, r, RGB(0, 0, 0), 70);
    else if (hover) Tint(di->hDC, r, di->CtlID == IDCANCEL ? kAccent : RGB(255, 255, 255), 45);
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT))
    {
        RECT f = r;
        InflateRect(&f, -3, -3);
        DrawFocusRect(di->hDC, &f);
    }
}

static HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
}

// Size the window to the art (scaled to fit the screen), paint the art and lay the controls out.
static void SetUpArtWindow(HWND wnd)
{
    Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_ISO_EDIT);
    Make(L"BUTTON", L"Browse", BS_OWNERDRAW | WS_TABSTOP, ID_ISO_BROWSE);
    Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_DIR_EDIT);
    Make(L"BUTTON", L"Browse", BS_OWNERDRAW | WS_TABSTOP, ID_DIR_BROWSE);
    Make(L"BUTTON", L"Create a &Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, ID_SC_DESKTOP);
    Make(L"BUTTON", L"Add to the &Start menu", BS_AUTOCHECKBOX | WS_TABSTOP, ID_SC_STARTMENU);
    Make(L"BUTTON", L"Install game", BS_OWNERDRAW | WS_TABSTOP, ID_INSTALL);
    Make(L"BUTTON", L"X", BS_OWNERDRAW | WS_TABSTOP, IDCANCEL);
    Make(PROGRESS_CLASSW, L"", 0, ID_PROGRESS);
    Make(L"STATIC", L"Select your own Splinter Cell: Double Agent (Xbox 360) disc image.", SS_LEFT | SS_ENDELLIPSIS, ID_STATUS);
    Make(L"STATIC", L"by 8gram Labs", SS_RIGHT, ID_CREDIT);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workW = work.right - work.left, workH = work.bottom - work.top;
    g_scale = std::min({ 1.0, 0.9 * workW / kArtW, 0.9 * workH / (kArtH + kStripH) });
    const int w = S(kArtW), artH = S(kArtH), h = S(kArtH + kStripH);
    SetWindowPos(wnd, nullptr, work.left + (workW - w) / 2, work.top + (workH - h) / 2, w, h, SWP_NOZORDER);

    g_fieldBrush = CreateSolidBrush(kFieldColor);
    g_stripBrush = CreateSolidBrush(kStripColor);
    g_fieldFont = CreateFontW(-std::max(12, S(19)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_uiFont = CreateFontW(-std::max(12, S(16)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");

    if (HBITMAP bmp = LoadArt(w, artH))
    {
        HDC screen = GetDC(nullptr);
        g_artDC = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        g_artBmp = bmp;
        g_artOld = (HBITMAP)SelectObject(g_artDC, g_artBmp);
        for (const RECT& f : kArtFields) { RECT r = S(f); FillRect(g_artDC, &r, g_fieldBrush); }
    }

    for (const ArtRect& a : kArtLayout)
    {
        const RECT r = S(a.r);
        HWND c = GetDlgItem(wnd, a.id);
        MoveWindow(c, r.left, r.top, r.right - r.left, r.bottom - r.top, FALSE);
        SetWindowSubclass(c, ArtButtonProc, 0, 0);
    }
    const int fieldTextH = std::max(12, S(19)) + 6;
    for (int i = 0; i < 2; ++i)
    {
        const RECT r = S(kArtFields[i]);
        const int cy = (r.top + r.bottom) / 2;
        HWND e = GetDlgItem(wnd, kFieldIds[i]);
        SendMessageW(e, WM_SETFONT, (WPARAM)g_fieldFont, FALSE);
        MoveWindow(e, r.left + S(10), cy - fieldTextH / 2, r.right - r.left - S(20), fieldTextH, FALSE);
    }
    SendDlgItemMessageW(wnd, ID_ISO_EDIT, EM_SETCUEBANNER, TRUE, (LPARAM)L"No ISO selected\x2026");

    // The strip, lined up with the Install button.
    const int left = S(420), right = S(1255);
    HWND progress = GetDlgItem(wnd, ID_PROGRESS);
    SetWindowTheme(progress, L"", L""); // unthemed, so it takes the colours below
    SendMessageW(progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(progress, PBM_SETBARCOLOR, 0, kAccent);
    SendMessageW(progress, PBM_SETBKCOLOR, 0, RGB(18, 30, 22));
    MoveWindow(progress, left, artH + S(12), right - left, std::max(6, S(10)), FALSE);
    const int rowH = std::max(12, S(16)) + 6;
    MoveWindow(GetDlgItem(wnd, ID_STATUS), left, artH + S(28), right - left, rowH, FALSE);
    int x = left;
    HDC dc = GetDC(wnd);
    HGDIOBJ of = SelectObject(dc, g_uiFont);
    for (int id : { ID_SC_DESKTOP, ID_SC_STARTMENU })
    {
        HWND c = GetDlgItem(wnd, id);
        SetWindowTheme(c, L"", L""); // themed check boxes ignore WM_CTLCOLORSTATIC's text colour
        CheckDlgButton(wnd, id, BST_CHECKED);
        wchar_t text[64];
        GetWindowTextW(c, text, 64);
        SIZE sz = {};
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
        const int cw = sz.cx + GetSystemMetrics(SM_CXMENUCHECK) + S(12);
        MoveWindow(c, x, artH + S(52), cw, rowH, FALSE);
        x += cw + S(24);
    }
    SelectObject(dc, of);
    ReleaseDC(wnd, dc);
    MoveWindow(GetDlgItem(wnd, ID_CREDIT), x, artH + S(52), right - x, rowH, FALSE);
    for (int id : { ID_SC_DESKTOP, ID_SC_STARTMENU, ID_STATUS, ID_CREDIT })
        SendDlgItemMessageW(wnd, id, WM_SETFONT, (WPARAM)g_uiFont, FALSE);
}

static void PaintArtBackground(HWND wnd, HDC dc)
{
    RECT c;
    GetClientRect(wnd, &c);
    const int artH = S(kArtH);
    if (g_artDC) BitBlt(dc, 0, 0, c.right, artH, g_artDC, 0, 0, SRCCOPY);
    else { RECT a = { 0, 0, c.right, artH }; FillRect(dc, &a, g_stripBrush); }
    RECT strip = { 0, artH, c.right, c.bottom };
    FillRect(dc, &strip, g_stripBrush);
    HBRUSH line = CreateSolidBrush(kAccent);
    RECT l = { 0, artH, c.right, artH + std::max(1, S(2)) };
    FillRect(dc, &l, line);
    DeleteObject(line);
}

static void FreeArt()
{
    if (g_artDC) { SelectObject(g_artDC, g_artOld); DeleteDC(g_artDC); g_artDC = nullptr; }
    if (g_artBmp) { DeleteObject(g_artBmp); g_artBmp = nullptr; }
    for (HGDIOBJ* o : { (HGDIOBJ*)&g_fieldFont, (HGDIOBJ*)&g_uiFont, (HGDIOBJ*)&g_fieldBrush, (HGDIOBJ*)&g_stripBrush })
        if (*o) { DeleteObject(*o); *o = nullptr; }
}

// ---- UI ----

static std::wstring GetText(int id)
{
    HWND h = GetDlgItem(g_wnd, id);
    std::wstring s(GetWindowTextLengthW(h), L'\0');
    GetWindowTextW(h, s.data(), (int)s.size() + 1);
    return s;
}

static std::wstring Pick(bool folder)
{
    std::wstring out;
    IFileOpenDialog* dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
    DWORD opts;
    dlg->GetOptions(&opts);
    if (folder)
    {
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        dlg->SetTitle(L"Choose where to install");
    }
    else
    {
        const COMDLG_FILTERSPEC f[] = { { L"Xbox 360 disc image (*.iso)", L"*.iso" }, { L"All files", L"*.*" } };
        dlg->SetOptions(opts | FOS_FILEMUSTEXIST);
        dlg->SetFileTypes(2, f);
        dlg->SetTitle(L"Choose your Splinter Cell: Double Agent disc image");
    }
    IShellItem* item;
    if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResult(&item)))
    {
        PWSTR p;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
        {
            out = p;
            CoTaskMemFree(p);
        }
        item->Release();
    }
    dlg->Release();
    return out;
}

static void SetControlsEnabled(bool on)
{
    for (int id : { ID_ISO_EDIT, ID_ISO_BROWSE, ID_DIR_EDIT, ID_DIR_BROWSE, ID_SC_DESKTOP, ID_SC_STARTMENU, ID_INSTALL })
        EnableWindow(GetDlgItem(g_wnd, id), on);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_wnd = hwnd;
        SetUpArtWindow(hwnd);
        SetDlgItemTextW(hwnd, ID_DIR_EDIT, (std::wstring(L"C:\\Games\\") + kFolderName).c_str());
        PostMessageW(hwnd, WM_APP_ASK, 0, 0);
        return 0;
    case WM_APP_ASK:
    {
        // Already installed: offer to update just scda.exe (no disc image needed).
        const std::wstring dir = FindInstall();
        if (dir.empty())
            return 0;
        SetDlgItemTextW(hwnd, ID_DIR_EDIT, dir.c_str());
        std::wstring have = ReadRecord(L"Version");
        if (have.empty()) have = L"unknown";
        const std::wstring q = std::wstring(kAppName) + L" is installed in " + dir + L"\n(installed version " + have +
            L", this installer " + kVersion + L").\n\nYes: update the game (keeps game data, settings and saves; no disc image needed)."
            L"\nNo: full reinstall from the disc image.";
        const int a = MessageBoxW(hwnd, q.c_str(), kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL)
            DestroyWindow(hwnd);
        else if (a == IDYES)
        {
            SetControlsEnabled(false);
            g_running = g_updating = true;
            g_cancel = false;
            g_error.clear();
            g_worker = std::thread(Update, dir);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        PaintArtBackground(hwnd, (HDC)wp);
        return 1;
    case WM_DRAWITEM:
        DrawArtButton(hwnd, (const DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        // Edits (enabled ones send CTLCOLOREDIT, disabled ones CTLCOLORSTATIC) sit on the drawn
        // fields; everything else is on the strip.
        HDC dc = (HDC)wp;
        const int id = GetDlgCtrlID((HWND)lp);
        const bool field = id == ID_ISO_EDIT || id == ID_DIR_EDIT;
        SetTextColor(dc, id == ID_CREDIT || (field && !IsWindowEnabled((HWND)lp)) ? kDimColor : kTextColor);
        SetBkColor(dc, field ? kFieldColor : kStripColor);
        return (LRESULT)(field ? g_fieldBrush : g_stripBrush);
    }
    case WM_LBUTTONDOWN: // no title bar: drag the window by the art
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_ISO_BROWSE:
        {
            std::wstring p = Pick(false);
            if (!p.empty()) SetDlgItemTextW(hwnd, ID_ISO_EDIT, p.c_str());
            break;
        }
        case ID_DIR_BROWSE:
        {
            std::wstring p = Pick(true);
            if (!p.empty()) SetDlgItemTextW(hwnd, ID_DIR_EDIT, (p + L"\\" + kFolderName).c_str());
            break;
        }
        case ID_INSTALL:
        {
            std::wstring iso = GetText(ID_ISO_EDIT), dir = GetText(ID_DIR_EDIT);
            while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
            if (iso.empty() || GetFileAttributesW(iso.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                MessageBoxW(hwnd, L"Choose your Splinter Cell: Double Agent disc image first.", kAppName, MB_ICONINFORMATION);
                break;
            }
            if (dir.size() < 3 || dir[1] != L':')
            {
                MessageBoxW(hwnd, L"Choose a full path to install to, for example C:\\Games\\SCDA-Recomp.", kAppName, MB_ICONINFORMATION);
                break;
            }
            SetControlsEnabled(false);
            g_running = true;
            g_cancel = false;
            g_error.clear();
            g_worker = std::thread(Install, iso, dir, IsDlgButtonChecked(hwnd, ID_SC_DESKTOP) == BST_CHECKED,
                                   IsDlgButtonChecked(hwnd, ID_SC_STARTMENU) == BST_CHECKED);
            break;
        }
        case IDCANCEL:
            if (g_running)
            {
                if (MessageBoxW(hwnd, L"Stop the installation?", kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES)
                    g_cancel = true;
                break;
            }
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_APP_STATUS:
    {
        auto* s = (std::wstring*)lp;
        SetDlgItemTextW(hwnd, ID_STATUS, s->c_str());
        delete s;
        return 0;
    }
    case WM_APP_PROGRESS:
        SendDlgItemMessageW(hwnd, ID_PROGRESS, PBM_SETPOS, wp, 0);
        return 0;
    case WM_APP_DONE:
        g_running = false;
        if (g_worker.joinable()) g_worker.join();
        if (wp)
        {
            std::wstring msg = std::wstring(kAppName) + (g_updating ? L" is updated." : L" is installed.") + L"\n\nPlay now?";
            if (MessageBoxW(hwnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES)
            {
                std::wstring dir = GetText(ID_DIR_EDIT);
                while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
                std::wstring game = L"\"" + dir + L"\\scda.exe\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", game.c_str(), dir.c_str(), SW_SHOWNORMAL);
            }
            DestroyWindow(hwnd);
        }
        else
        {
            SetDlgItemTextW(hwnd, ID_STATUS, L"");
            SendDlgItemMessageW(hwnd, ID_PROGRESS, PBM_SETPOS, 0, 0);
            MessageBoxW(hwnd, g_error.empty() ? L"The installation did not finish." : g_error.c_str(), kAppName, MB_ICONERROR);
            g_updating = false;
            SetControlsEnabled(true);
        }
        return 0;
    case WM_CLOSE:
        SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
        return 0;
    case WM_DESTROY:
        FreeArt();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Headless: SCDA_Setup.exe --iso <image> --dir <folder> [--no-shortcut]
    //           SCDA_Setup.exe --update [--dir <folder>]   (default: the recorded/shortcut install)
    //             [--wait-pid <pid>] first waits (60 s max) for that process (the game's in-game
    //             updater) to exit; [--relaunch] starts scda.exe again after a successful update.
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring iso, dir;
    bool shortcut = true, update = false, relaunch = false;
    DWORD waitPid = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--iso") && i + 1 < argc) iso = argv[++i];
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dir = argv[++i];
        else if (!wcscmp(argv[i], L"--no-shortcut")) shortcut = false;
        else if (!wcscmp(argv[i], L"--update")) update = true;
        else if (!wcscmp(argv[i], L"--wait-pid") && i + 1 < argc) waitPid = wcstoul(argv[++i], nullptr, 10);
        else if (!wcscmp(argv[i], L"--relaunch")) relaunch = true;
    }
    if (update && waitPid)
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, waitPid))
        {
            WaitForSingleObject(p, 60000);
            CloseHandle(p);
            Sleep(500); // the exe image is released just after the process object signals
        }
    if (update)
    {
        g_headless = true;
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            freopen("CONOUT$", "w", stdout);
        if (dir.empty()) dir = FindInstall();
        if (dir.empty()) { PostStatus(L"No installed game was found."); return 1; }
        Update(dir);
        if (!g_ok && relaunch) // started from the game: nobody sees the console, so say why
            MessageBoxW(nullptr, g_error.c_str(), L"SCDA Recomp update", MB_ICONWARNING);
        if (g_ok && relaunch)
            ShellExecuteW(nullptr, L"open", (dir + L"\scda.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        return g_ok ? 0 : 1;
    }
    if (!iso.empty() && !dir.empty())
    {
        g_headless = true;
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            freopen("CONOUT$", "w", stdout);
        Install(iso, dir, shortcut, shortcut);
        return g_ok ? 0 : 1;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SCDARecompSetup";
    RegisterClassExW(&wc);

    // Borderless, painted with the art; the caption is kept for the taskbar only.
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Install Splinter Cell: Double Agent (Recomp)",
        WS_POPUP | WS_CLIPCHILDREN | WS_SYSMENU | WS_MINIMIZEBOX, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(hwnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (g_worker.joinable())
    {
        g_cancel = true;
        g_worker.join();
    }
    return 0;
}
