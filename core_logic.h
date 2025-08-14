#ifndef CORE_LOGIC_H
#define CORE_LOGIC_H

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
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

// The main class for handling MRTK operations
class MRTKToolCore {
public:
    MRTKToolCore();

    // Fetches the list of available MRTK and OpenXR packages from GitHub.
    bool fetchAvailablePackages();

    // Returns the list of fetched packages.
    const std::vector<SelectablePackage>& getAvailablePackages() const;
    
    // Resolves all necessary dependencies for a given list of selected packages.
    void resolveDependencies(const std::vector<int>& selectedIndices);
    
    // Downloads the resolved packages and places them in a temporary "MixedReality" folder.
    void downloadAndRepackage();
    
    // Moves the downloaded packages into the Unity project and updates the manifest.json.
    void installPackagesToProject(const fs::path& projectPath);

    // Static helper to check if a path points to a valid Unity project.
    static bool isValidUnityProject(const fs::path& path);

    // Static helper to get the Unity version from a project.
    static std::string getUnityVersion(const fs::path& projectPath);

    std::map<std::string, std::string> resolvedUserSelections;
    std::map<std::string, std::string> resolvedDependencies;

private:
    // ---- PRIVATE HELPER METHODS ----
    std::string httpGet(const std::string& url);
    std::string downloadFile(const std::string& url, const std::string& outputPath = "");
    void extractArchive(const std::string& archiveFile, const std::string& extractPath);
    void createTgzArchive(const fs::path& folderPath, const fs::path& tgzFileName);
    std::map<std::string, std::string> getDependenciesFromTgz(const std::string& tgzFilePath);
    void extractAndRepackageGraphicsTools(const std::string& downloadedFile, const std::string& version);
    std::pair<std::string, std::string> extractComponentInfo(const std::string& fileName);
    std::string findDownloadUrlForComponent(const std::string& component_name, const std::string& version);
    void resolveDependenciesRecursive(const std::string& component, const std::string& version, std::set<std::string>& processedComponents);
    static bool isNewerVersion(const std::string& v_old, const std::string& v_new);

    // ---- PRIVATE MEMBER VARIABLES ----
    nlohmann::json githubJsonParsed;
    std::vector<SelectablePackage> allPackages;
    std::map<std::string, std::vector<std::string>> mrtkComponentVersions;
    std::map<std::string, std::string> requiredMrtkPackages; // Final list of MRTK packages to download
    std::set<std::string> requiredOpenXrPackages; // Final list of OpenXR packages for manifest
};

#endif // CORE_LOGIC_H