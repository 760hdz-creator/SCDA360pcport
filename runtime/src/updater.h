// Update check (F1 menu, Update tab, and a notification at startup): the latest release of the
// port's GitLab project and its GitHub repository, both asked, higher version wins (SCDA_UPDATE_REPO, group/name; [Update] Repo= in settings.ini overrides
// it), compared with this build's version (SCDA_VERSION). Only a strictly higher version counts as
// an update. InstallUpdate downloads the release's SCDA_Setup.exe and runs its update mode.
#pragma once
#include <string>

namespace updater {

enum class State { Idle, Checking, UpToDate, Available, NoReleases, Failed };

struct Status
{
    State state = State::Idle;
    std::string latest; // the release's version ("0.2.0", tag without a leading v), once checked
    std::string title;  // the release's name
    std::string error;  // Failed: why
    std::string installerUrl; // the release's SCDA_Setup.exe download, empty if it has none
    std::string fallbackUrl;  // the other source's installer for the same version, if any
    std::string source;       // where the offered release came from: "GitLab" / "GitHub"
};

const char* CurrentVersion();
const std::string& Repo();       // GitLab group/name
const std::string& GitHubRepo(); // GitHub owner/name, also asked (SCDA_UPDATE_GITHUB_REPO; empty = not asked)
// https://gitlab.com/<repo>/-/releases/permalink/latest.
std::string ReleasePageUrl();
Status Get();
void Check(); // in the background; ignored while a check runs
void OpenReleasePage();
// Blocking: downloads the available release's SCDA_Setup.exe to %TEMP% and starts it with
// --update --dir <this exe's folder> --wait-pid <this process> --relaunch. True when the installer
// started: the caller then exits so the installer can replace scda.exe.
bool InstallUpdate(std::string& error);

// Pure helpers (tests).
// <0, 0, >0 like strcmp; numeric fields ("0.10.0" > "0.9.1"), a leading v ignored, missing fields 0.
int CompareVersions(const std::string& a, const std::string& b);
// The string value of "key" in a JSON object (no nesting needed), unescaped; empty if absent.
std::string JsonString(const std::string& json, const std::string& key);

} // namespace updater
