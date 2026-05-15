#ifndef CORE_LOGIC_H
#define CORE_LOGIC_H

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <functional>
#include <filesystem>
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

// Enum to differentiate package types
enum class PackageType { MRTK, OpenXR };

// Structure to hold information about a selectable package
struct SelectablePackage {
    std::string displayName;      // User-friendly name, e.g., "MRTK Core"
    std::string identifier;       // Technical name, e.g., "core" or "com.microsoft.mixedreality.openxr"
    PackageType type;             // The type of the package
};

// Structure for comparing Unity versions
struct UnityVersion {
    int major = 0, minor = 0, patch = 0;
    char type = 'f';
    int build = 0;

    UnityVersion(const std::string& version_str);
    bool operator>(const UnityVersion& other) const;
};

// Optional progress sink: GUI passes a lambda that marshals lines to wxWidgets;
// CLI leaves it unset and the core logs to std::cout.
using ProgressCallback = std::function<void(const std::string&)>;

// Splits "<package-name>-<version>.tgz" into {name, version}. Returns empty pair on no match.
// Handles plain (1.2.3) and prerelease (1.2.3-pre.4) Microsoft package versions.
std::pair<std::string, std::string> parsePackageNameFromTgz(const std::string& fileName);

// The main class for handling MRTK operations
class MRTKToolCore {
public:
    MRTKToolCore();

    void setProgressCallback(ProgressCallback cb);

    // Fetches the list of available MRTK and OpenXR packages from GitHub.
    bool fetchAvailablePackages();

    // Returns the list of fetched packages.
    const std::vector<SelectablePackage>& getAvailablePackages() const;

    // Resolves all necessary dependencies for a given list of selected packages.
    void resolveDependencies(const std::vector<int>& selectedIndices);

    // Downloads the resolved packages into a per-instance temp folder.
    void downloadAndRepackage();

    // Copies the downloaded packages into the Unity project and updates the manifest.json.
    void installPackagesToProject(const fs::path& projectPath);

    // Static helper to check if a path points to a valid Unity project.
    static bool isValidUnityProject(const fs::path& path);

    // Static helper to get the Unity version from a project.
    static std::string getUnityVersion(const fs::path& projectPath);

    std::map<std::string, std::string> resolvedUserSelections;
    std::map<std::string, std::string> resolvedDependencies;

private:
    // ---- PRIVATE HELPER METHODS ----
    void log(const std::string& message) const;
    std::string httpGet(const std::string& url);
    std::string downloadFile(const std::string& url, const fs::path& outputPath = {});
    bool extractArchive(const fs::path& archiveFile, const fs::path& extractPath);
    bool createTgzArchive(const fs::path& folderPath, const fs::path& tgzFileName);
    std::map<std::string, std::string> getDependenciesFromTgz(const fs::path& tgzFilePath);
    void extractAndRepackageGraphicsTools(const fs::path& downloadedFile, const std::string& version);
    std::string findDownloadUrlForComponent(const std::string& component_name, const std::string& version);
    void resolveDependenciesRecursive(const std::string& component, const std::string& version, std::set<std::string>& processedComponents);
    static bool isNewerVersion(const std::string& v_old, const std::string& v_new);

    // ---- PRIVATE MEMBER VARIABLES ----
    nlohmann::json githubJsonParsed;
    std::vector<SelectablePackage> allPackages;
    std::map<std::string, std::vector<std::string>> mrtkComponentVersions;
    std::map<std::string, std::string> requiredMrtkPackages; // Final list of MRTK packages to download
    std::set<std::string> requiredOpenXrPackages; // Final list of OpenXR packages for manifest

    fs::path workDir;                          // Per-instance temp directory for downloads.
    std::vector<fs::path> downloadedFiles;     // Tgz paths produced this run; consumed by install.
    ProgressCallback progressCallback;
};

#endif // CORE_LOGIC_H