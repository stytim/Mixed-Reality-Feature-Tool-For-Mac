// Lightweight regression harness for P0/P1 bugs fixed in the audit.
// No external test framework — failures produce non-zero exit code.

#include "core_logic.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int g_failures = 0;

#define EXPECT(cond, msg)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::cerr << "FAIL: " << __func__ << ":" << __LINE__       \
                      << " " << (msg) << " — `" #cond "` was false\n"; \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

void test_parse_handles_graphics_tools_prerelease() {
    auto [name, ver] = parsePackageNameFromTgz("com.microsoft.mrtk.graphicstools.unity-1.0.0-pre.1.tgz");
    EXPECT(name == "com.microsoft.mrtk.graphicstools.unity", "graphics-tools name");
    EXPECT(ver  == "1.0.0-pre.1", "graphics-tools version");
}

void test_parse_handles_plain_mrtk_release() {
    auto [name, ver] = parsePackageNameFromTgz("org.mixedrealitytoolkit.core-3.0.0.tgz");
    EXPECT(name == "org.mixedrealitytoolkit.core", "mrtk name");
    EXPECT(ver  == "3.0.0", "mrtk version");
}

void test_parse_rejects_garbage() {
    auto [name, ver] = parsePackageNameFromTgz("README.md");
    EXPECT(name.empty(), "garbage filename → empty name");
    EXPECT(ver.empty(),  "garbage filename → empty version");
    auto [n2, v2] = parsePackageNameFromTgz("foo.tgz");
    EXPECT(n2.empty(), "no-version tgz → empty name");
}

void test_unity_version_parser_handles_malformed() {
    // None of these should throw.
    try {
        UnityVersion v1("");
        UnityVersion v2("garbage");
        UnityVersion v3("1.x.0f0");
        UnityVersion v4("1.0");
        (void)v1; (void)v2; (void)v3; (void)v4;
    } catch (const std::exception& e) {
        EXPECT(false, std::string("UnityVersion ctor threw: ") + e.what());
    }
}

void test_unity_version_ordering() {
    UnityVersion u6{"6000.0.0f0"};
    UnityVersion u22{"2022.3.0f1"};
    UnityVersion u22b{"2022.3.0f1"};
    UnityVersion u_older{"2022.2.10f1"};
    EXPECT(u6 > u22, "6000 > 2022.3");
    EXPECT(u22 > u_older, "2022.3 > 2022.2.10");
    EXPECT(!(u22 > u22b), "equal versions not >");
}

void test_get_unity_version_handles_malformed_line() {
    // Smoke-check using a temp ProjectVersion.txt to confirm regex parser
    // returns the expected value and does not regress to returning the whole line.
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "mrtk_test_unity_proj";
    fs::create_directories(tmp / "ProjectSettings");
    {
        std::ofstream f(tmp / "ProjectSettings" / "ProjectVersion.txt");
        f << "m_EditorVersion: 2022.3.10f1\n";
        f << "m_EditorVersionWithRevision: 2022.3.10f1 (abc123)\n";
    }
    std::string v = MRTKToolCore::getUnityVersion(tmp);
    EXPECT(v == "2022.3.10f1", std::string("expected 2022.3.10f1, got '") + v + "'");
    fs::remove_all(tmp);
}
} // namespace

int main() {
    test_parse_handles_graphics_tools_prerelease();
    test_parse_handles_plain_mrtk_release();
    test_parse_rejects_garbage();
    test_unity_version_parser_handles_malformed();
    test_unity_version_ordering();
    test_get_unity_version_handles_malformed_line();

    if (g_failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test failure(s).\n";
    return 1;
}
