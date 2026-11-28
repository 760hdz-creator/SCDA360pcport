#include "check.h"
#include <updater.h>

TEST(updater_compare_versions)
{
    CHECK(updater::CompareVersions("0.2.0", "0.1.9") > 0);
    CHECK(updater::CompareVersions("v0.10.0", "0.9.1") > 0);
    CHECK(updater::CompareVersions("1.0", "1.0.0") == 0);
    CHECK(updater::CompareVersions("0.1.0", "v0.1.0") == 0);
    CHECK(updater::CompareVersions("0.1.0-rc1", "0.1.0") == 0);
    CHECK(updater::CompareVersions("0.1.0", "0.1.1") < 0);
}

TEST(updater_json_string)
{
    const std::string json = R"({"url":"x","author":{"login":"someone"},"tag_name": "v0.2.0","name":"First \"public\" build","draft":false,
        "assets":[{"name":"SCDA_Setup.exe"}]})";
    CHECK_EQ(updater::JsonString(json, "tag_name"), std::string("v0.2.0"));
    CHECK_EQ(updater::JsonString(json, "name"), std::string("First \"public\" build"));
    CHECK_EQ(updater::JsonString(json, "body"), std::string());
    CHECK_EQ(updater::JsonString(json, "draft"), std::string()); // not a string
}

TEST(updater_forge_release_json)
{
    // Shape of git.8gramlabs.com/api/v1/repos/<owner>/<repo>/releases/latest (Forgejo), trimmed.
    const std::string json = R"({"id":7,"tag_name":"v0.1.16","name":"SCDA Recomp 0.1.16","draft":false,"prerelease":false,)"
        R"("assets":[{"id":3,"name":"SCDA_Setup.exe","size":59975680,"download_count":20,"created_at":"2026-11-12T05:12:56Z",)"
        R"("uuid":"66719a3b-6a42-40fa-bb7e-e336d2386087","browser_download_url":"https://git.8gramlabs.com/8gramlabs/SCDA-Recomp/releases/download/v0.1.16/SCDA_Setup.exe","type":"attachment"}]})";
    CHECK_EQ(updater::JsonString(json, "tag_name"), std::string("v0.1.16"));
    const size_t name = json.find("\"name\":\"SCDA_Setup.exe\"");
    CHECK(name != std::string::npos);
    CHECK_EQ(updater::JsonString(json.substr(name, 4096), "browser_download_url"),
             std::string("https://git.8gramlabs.com/8gramlabs/SCDA-Recomp/releases/download/v0.1.16/SCDA_Setup.exe"));
}
