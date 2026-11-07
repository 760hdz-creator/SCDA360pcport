#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <vector>
#include <runtime.h>
#include <settings.h>
#include "updater.h"

#ifndef SCDA_VERSION
#define SCDA_VERSION "0.0.0"
#endif
#ifndef SCDA_UPDATE_GITHUB_REPO
#define SCDA_UPDATE_GITHUB_REPO "8gramlabs/SCDA360pcport"
#endif
#ifndef SCDA_UPDATE_REPO
#define SCDA_UPDATE_REPO "octagram-labs-group/SCDA360pcport"
#endif

namespace updater {

namespace {

std::mutex g_mutex;
Status g_status;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// GET an https URL (redirects followed); the body goes to `body` (max 1 MB) or, with `file`, to that
// file. False with `error` set when no HTTP answer came back.
bool HttpGet(const std::string& url, DWORD& statusCode, std::string& body, std::string& error, HANDLE file = nullptr)
{
    const std::wstring wurl = Widen(url);
    wchar_t host[256] = {}, path[2048] = {};
    URL_COMPONENTS uc{ sizeof(uc) };
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
    {
        error = "bad URL " + url;
        return false;
    }
    const std::wstring agent = Widen(std::string("SCDARecomp/") + SCDA_VERSION);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = false;
    if (request)
    {
        WinHttpSetTimeouts(request, 5000, 5000, 10000, file ? 60000 : 10000);
        DWORD size = sizeof(statusCode);
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
                                WINHTTP_NO_HEADER_INDEX))
        {
            ok = true;
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail) || !avail || (!file && body.size() > (1u << 20)))
                    break;
                std::vector<char> chunk(avail);
                DWORD read = 0, written = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read) || !read)
                    break;
                if (!file)
                    body.append(chunk.data(), read);
                else if (!WriteFile(file, chunk.data(), read, &written, nullptr) || written != read)
                {
                    ok = false;
                    break;
                }
            }
        }
    }
    if (!ok)
        error = "no connection to " + std::string(host, host + wcslen(host)) + " (error " + std::to_string(GetLastError()) + ")";
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    if (session)
        WinHttpCloseHandle(session);
    return ok;
}

std::string UrlEncode(const std::string& s)
{
    std::string out;
    char buf[4];
    for (unsigned char c : s)
        if (isalnum(c) || c == '-' || c == '_' || c == '.')
            out += char(c);
        else
        {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    return out;
}

// The installer URL in a release JSON: the value of `key` after the asset named SCDA_Setup.exe
// (GitLab assets.links[] direct_asset_url; GitHub assets[] browser_download_url, which follows a
// nested "uploader" object, so this looks ahead rather than to the object's end). Empty if none.
std::string InstallerUrl(const std::string& json, const std::string& key)
{
    const size_t name = json.find("\"name\":\"SCDA_Setup.exe\"");
    if (name == std::string::npos)
        return {};
    return JsonString(json.substr(name, 4096), key);
}

void Set(const Status& s)
{
    std::lock_guard lock(g_mutex);
    g_status = s;
}

// One source's latest release.
struct Candidate
{
    std::string source; // "GitLab" / "GitHub"
    bool ok = false;    // answered with a tagged release
    bool none = false;  // answered: no release published
    std::string version, title, installerUrl, error;
};

Candidate Fetch(bool gitlab)
{
    Candidate c;
    c.source = gitlab ? "GitLab" : "GitHub";
    const std::string url = gitlab ? "https://gitlab.com/api/v4/projects/" + UrlEncode(Repo()) + "/releases/permalink/latest"
                                   : "https://api.github.com/repos/" + GitHubRepo() + "/releases/latest";
    DWORD code = 0;
    std::string body;
    if (!HttpGet(url, code, body, c.error))
        return c;
    if (code == 404)
    {
        c.none = true; // no release, or the project is gone / private
        return c;
    }
    if (code != 200)
    {
        c.error = c.source + " answered " + std::to_string(code) + (code == 403 || code == 429 ? " (rate limit, try later)" : "");
        return c;
    }
    c.version = JsonString(body, "tag_name");
    if (!c.version.empty() && (c.version[0] == 'v' || c.version[0] == 'V'))
        c.version.erase(0, 1);
    if (c.version.empty())
    {
        c.error = c.source + "'s release has no tag";
        return c;
    }
    c.title = JsonString(body, "name");
    c.installerUrl = InstallerUrl(body, gitlab ? "direct_asset_url" : "browser_download_url");
    c.ok = true;
    return c;
}

// Asks GitLab and GitHub at once (either may be down or gone); the higher version wins, and for the
// same version GitLab (the project's home) unless only GitHub has the installer. The other source
// with the same version stays as the download fallback.
void RunCheck()
{
    Candidate lab, hub;
    std::thread labThread([&] { lab = Fetch(true); });
    if (!GitHubRepo().empty())
        hub = Fetch(false);
    labThread.join();
    for (const Candidate* c : { &lab, &hub })
        if (!c->source.empty())
            RuntimeLog("[update] %s: %s\n", c->source.c_str(), c->ok ? ("release " + c->version + (c->installerUrl.empty() ? ", no installer" : ", installer")).c_str()
                                                                  : c->none ? "no release" : c->error.c_str());
    std::vector<const Candidate*> offers;
    for (const Candidate* c : { &lab, &hub })
        if (c->ok)
            offers.push_back(c);
    std::stable_sort(offers.begin(), offers.end(), [](const Candidate* x, const Candidate* y)
    {
        if (const int v = CompareVersions(x->version, y->version))
            return v > 0;
        return !x->installerUrl.empty() && y->installerUrl.empty(); // same version: the one with an installer
    });
    Status s;
    if (offers.empty())
    {
        const bool none = lab.none && (hub.source.empty() || hub.none || !hub.error.empty());
        s.state = none ? State::NoReleases : State::Failed;
        s.error = lab.error.empty() ? hub.error : hub.error.empty() || hub.source.empty() ? lab.error : "GitLab: " + lab.error + "; GitHub: " + hub.error;
    }
    else
    {
        const Candidate& best = *offers[0];
        s.latest = best.version;
        s.title = best.title;
        s.source = best.source;
        s.installerUrl = best.installerUrl;
        if (offers.size() > 1 && CompareVersions(offers[1]->version, best.version) == 0)
            s.fallbackUrl = offers[1]->installerUrl;
        s.state = CompareVersions(s.latest, CurrentVersion()) > 0 ? State::Available : State::UpToDate;
    }
    RuntimeLog("[update] state %d latest '%s' from %s%s%s\n", int(s.state), s.latest.c_str(), s.source.empty() ? "-" : s.source.c_str(),
        s.error.empty() ? "" : " ", s.error.c_str());
    Set(s);
}

} // namespace

const char* CurrentVersion()
{
    static const char* fake = getenv("SCDA_FAKE_VERSION"); // tests: pretend to be an older build
    return fake ? fake : SCDA_VERSION;
}

const std::string& Repo()
{
    static const std::string repo = []
    {
        std::string r = LoadUpdateSettings(SettingsPath()).repo;
        return r.empty() ? std::string(SCDA_UPDATE_REPO) : r;
    }();
    return repo;
}

const std::string& GitHubRepo()
{
    // SCDA_NO_GITHUB_UPDATE=1 skips GitHub; SCDA_UPDATE_GITHUB_REPO=<owner/name> asks another repo (tests).
    static const std::string repo = getenv("SCDA_NO_GITHUB_UPDATE") ? std::string()
                                  : getenv("SCDA_UPDATE_GITHUB_REPO") ? std::string(getenv("SCDA_UPDATE_GITHUB_REPO"))
                                                                      : std::string(SCDA_UPDATE_GITHUB_REPO);
    return repo;
}

std::string ReleasePageUrl() { return "https://gitlab.com/" + Repo() + "/-/releases/permalink/latest"; }

Status Get()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}

void Check()
{
    {
        std::lock_guard lock(g_mutex);
        if (g_status.state == State::Checking)
            return;
        g_status = Status{ State::Checking };
    }
    std::thread(RunCheck).detach();
}

void OpenReleasePage()
{
    const std::wstring url = Widen(ReleasePageUrl());
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int CompareVersions(const std::string& a, const std::string& b)
{
    size_t i = !a.empty() && (a[0] == 'v' || a[0] == 'V'), j = !b.empty() && (b[0] == 'v' || b[0] == 'V');
    while (i < a.size() || j < b.size())
    {
        unsigned long x = 0, y = 0;
        while (i < a.size() && isdigit((unsigned char)a[i]))
            x = x * 10 + unsigned(a[i++] - '0');
        while (j < b.size() && isdigit((unsigned char)b[j]))
            y = y * 10 + unsigned(b[j++] - '0');
        if (x != y)
            return x < y ? -1 : 1;
        // Next field: past the '.', stop at anything else (a "-rc1" suffix is ignored).
        i = i < a.size() && a[i] == '.' ? i + 1 : a.size();
        j = j < b.size() && b[j] == '.' ? j + 1 : b.size();
    }
    return 0;
}

std::string JsonString(const std::string& json, const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    size_t p = json.find(quoted);
    if (p == std::string::npos)
        return {};
    p = json.find_first_not_of(" \t\r\n", p + quoted.size());
    if (p == std::string::npos || json[p] != ':')
        return {};
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '"')
        return {};
    std::string out;
    for (p++; p < json.size() && json[p] != '"'; p++)
    {
        if (json[p] == '\\' && p + 1 < json.size())
        {
            const char e = json[++p];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'u' ? '?' : e;
            if (e == 'u')
                p += 4; // non-ASCII in a release name: shown as '?'
        }
        else
            out += json[p];
    }
    return out;
}

bool InstallUpdate(std::string& error)
{
    const Status st = Get();
    if (st.state != State::Available || st.installerUrl.empty())
    {
        error = "no downloadable update";
        return false;
    }
    wchar_t temp[MAX_PATH], exe[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring setup = std::wstring(temp) + L"SCDA_Setup_update.exe";
    HANDLE file = CreateFileW(setup.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "cannot write the installer to %TEMP%";
        return false;
    }
    // The chosen source first, then the other one offering the same version. The file must be a
    // Windows program ("MZ", over 1 MB): neither source gives a checksum we check.
    bool done = false;
    for (const std::string& url : { st.installerUrl, st.fallbackUrl })
    {
        if (url.empty() || done)
            continue;
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
        DWORD code = 0;
        std::string body;
        const bool got = HttpGet(url, code, body, error, file);
        char mz[2] = {};
        DWORD read = 0;
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        ReadFile(file, mz, 2, &read, nullptr);
        done = got && code == 200 && GetFileSize(file, nullptr) > (1u << 20) && mz[0] == 'M' && mz[1] == 'Z';
        if (!done)
        {
            if (got)
                error = "download failed (HTTP " + std::to_string(code) + ")";
            RuntimeLog("[update] download %s: %s\n", url.c_str(), error.c_str());
        }
    }
    CloseHandle(file);
    if (!done)
    {
        DeleteFileW(setup.c_str());
        return false;
    }
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/"));
    // The installer waits for this process to exit, swaps scda.exe, then starts the game again.
    const std::wstring args = L"--update --dir \"" + dir + L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId()) + L" --relaunch";
    RuntimeLog("[update] installing %s (%s)\n", st.latest.c_str(), st.source.c_str());
    if (intptr_t(ShellExecuteW(nullptr, L"open", setup.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
    {
        error = "could not start the installer";
        return false;
    }
    return true;
}

} // namespace updater
