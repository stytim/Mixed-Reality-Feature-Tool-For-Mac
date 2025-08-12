#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <regex>

#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

// ---- START: New Package Structures ----
enum class PackageType { MRTK, OpenXR };

struct SelectablePackage {
    std::string displayName;      // e.g., "core" or "Microsoft Mixed Reality OpenXR"
    std::string identifier;       // e.g., "core" (for MRTK) or "com.microsoft.mixedreality.openxr"
    PackageType type;
};
// ---- END: New Package Structures ----

// Forward declarations
void downloadAndProcessDependencies(
    const std::string& component,
    const std::string& version,
    const nlohmann::json& jsonParsed,
    std::set<std::string>& processedComponents,
    std::map<std::string, std::string>& downloadedComponents,
    const std::map<std::string, std::function<void(const std::string&, const std::string&, std::map<std::string, std::string>&)>>& customHandlers);

std::string downloadFile(const std::string& url);
void extractAndRepackage(const std::string& downloadedFile, const std::string& version);

// A more robust semantic versioning comparison function.
// Returns true if v_new is newer than v_old.
bool isNewerVersion(const std::string& v_old, const std::string& v_new) {
    std::vector<int> old_parts, new_parts;
    std::string temp;

    size_t old_pre_pos = v_old.find('-');
    size_t new_pre_pos = v_new.find('-');

    std::string old_main = v_old.substr(0, old_pre_pos);
    std::string new_main = v_new.substr(0, new_pre_pos);

    std::stringstream ss_old(old_main);
    while (getline(ss_old, temp, '.')) { old_parts.push_back(std::stoi(temp)); }

    std::stringstream ss_new(new_main);
    while (getline(ss_new, temp, '.')) { new_parts.push_back(std::stoi(temp)); }

    size_t min_len = std::min(old_parts.size(), new_parts.size());
    for (size_t i = 0; i < min_len; ++i) {
        if (new_parts[i] > old_parts[i]) return true;
        if (new_parts[i] < old_parts[i]) return false;
    }

    if (new_parts.size() > old_parts.size()) return true;
    if (old_parts.size() > new_parts.size()) return false;

    // Main versions (e.g., 3.2.2) are identical. Now check pre-release tags.
    bool old_is_pre = (old_pre_pos != std::string::npos);
    bool new_is_pre = (new_pre_pos != std::string::npos);

    if (old_is_pre && !new_is_pre) return true;  // A stable release is newer than a pre-release.
    if (!old_is_pre && new_is_pre) return false; // A pre-release is not newer than a stable release.
    if (!old_is_pre && !new_is_pre) return false; // Both are stable and identical.

    // Both are pre-releases. Compare the tags (e.g., "pre.18" vs "pre.20").
    std::string old_tag = v_old.substr(old_pre_pos + 1);
    std::string new_tag = v_new.substr(new_pre_pos + 1);

    std::vector<std::string> old_tag_parts, new_tag_parts;
    std::stringstream ss_old_tag(old_tag);
    while (getline(ss_old_tag, temp, '.')) { old_tag_parts.push_back(temp); }

    std::stringstream ss_new_tag(new_tag);
    while (getline(ss_new_tag, temp, '.')) { new_tag_parts.push_back(temp); }

    size_t min_tag_len = std::min(old_tag_parts.size(), new_tag_parts.size());
    for (size_t i = 0; i < min_tag_len; ++i) {
        bool old_part_is_num = !old_tag_parts[i].empty() && std::all_of(old_tag_parts[i].begin(), old_tag_parts[i].end(), ::isdigit);
        bool new_part_is_num = !new_tag_parts[i].empty() && std::all_of(new_tag_parts[i].begin(), new_tag_parts[i].end(), ::isdigit);

        if (old_part_is_num && new_part_is_num) {
            int old_num = std::stoi(old_tag_parts[i]);
            int new_num = std::stoi(new_tag_parts[i]);
            if (new_num > old_num) return true;
            if (new_num < old_num) return false;
        } else {
            if (new_tag_parts[i] > old_tag_parts[i]) return true;
            if (new_tag_parts[i] < old_tag_parts[i]) return false;
        }
    }
    
    return new_tag_parts.size() > old_tag_parts.size();
}

struct UnityVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    char type = 'f';
    int build = 0;

    UnityVersion(const std::string& version_str) {
        if (version_str.empty()) return;
        std::sscanf(version_str.c_str(), "%d.%d.%d", &major, &minor, &patch);
        size_t type_pos = version_str.find_first_of("abfp");
        if (type_pos != std::string::npos) {
            type = version_str[type_pos];
            build = std::stoi(version_str.substr(type_pos + 1));
        }
    }
    
    bool operator>(const UnityVersion& other) const {
        if (major != other.major) return major > other.major;
        if (minor != other.minor) return minor > other.minor;
        if (patch != other.patch) return patch > other.patch;
        if (type != other.type) return type > other.type; 
        if (build != other.build) return build > other.build;
        return false;
    }
};

std::string getUnityVersion(const fs::path& projectPath) {
    fs::path version_file = projectPath / "ProjectSettings" / "ProjectVersion.txt";
    if (!fs::exists(version_file)) {
        return "";
    }
    std::ifstream file(version_file);
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("m_EditorVersion:", 0) == 0) {
            return line.substr(line.find(' ') + 1);
        }
    }
    return "";
}

static size_t WriteDataCallback(void* buffer, size_t size, size_t nmemb, void* userp) {
    auto* outputFile = static_cast<std::ofstream*>(userp);
    size_t actualSize = size * nmemb;
    outputFile->write(static_cast<char*>(buffer), actualSize);
    return actualSize;
}

static size_t WriteStringCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* s = static_cast<std::string*>(userp);
    size_t newLength = size * nmemb;
    try {
        s->append(static_cast<char*>(contents), newLength);
        return newLength;
    } catch (std::bad_alloc& e) {
        return 0;
    }
}

CURLcode performCurlRequest(const std::string& url, void* write_data, size_t (*write_function)(void*, size_t, size_t, void*)) {
    CURL* curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_function);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_data);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "curl_easy_perform() failed for URL " << url << ": " << curl_easy_strerror(res) << std::endl;
    }
    curl_easy_cleanup(curl);
    return res;
}

std::string downloadFile(const std::string& url) {
    fs::path urlPath(url);
    std::string filename = urlPath.filename().string();
    if (filename.empty()) {
        std::cerr << "Error: Could not extract file name from URL." << std::endl;
        return "";
    }
    std::ofstream outputFile(filename, std::ios::binary);
    if (!outputFile) {
        std::cerr << "Error: Cannot open file " << filename << " for writing." << std::endl;
        return "";
    }
    if (performCurlRequest(url, &outputFile, WriteDataCallback) != CURLE_OK) return "";
    return filename;
}

std::string httpGet(const std::string& url) {
    std::string readBuffer;
    performCurlRequest(url, &readBuffer, WriteStringCallback);
    return readBuffer;
}

void extractArchive(const std::string& archiveFile, const std::string& extractPath) {
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, archiveFile.c_str(), 10240) != ARCHIVE_OK) {
        std::cerr << "Error opening archive: " << archive_error_string(a) << std::endl;
        archive_read_free(a);
        return;
    }
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        fs::path fullPath = fs::path(extractPath) / archive_entry_pathname(entry);
        fs::create_directories(fullPath.parent_path());
        if (archive_entry_filetype(entry) == AE_IFREG) {
            std::ofstream outputFile(fullPath, std::ios::binary);
            if (!outputFile) {
                std::cerr << "Error: Cannot open file " << fullPath.string() << " for writing." << std::endl;
                continue;
            }
            const void* buff;
            size_t size;
            la_int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                outputFile.write(static_cast<const char*>(buff), size);
            }
        }
    }
    archive_read_free(a);
}

void createTgzArchive(const fs::path& folderPath, const fs::path& tgzFileName) {
    struct archive* a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    archive_write_open_filename(a, tgzFileName.string().c_str());
    std::string dirName = folderPath.filename().string();
    for (const auto& file : fs::recursive_directory_iterator(folderPath)) {
        const std::string relativePath = fs::relative(file.path(), folderPath).string();
        fs::path archivePath = fs::path(dirName) / relativePath;
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, archivePath.string().c_str());
        archive_entry_set_size(entry, file.is_regular_file() ? file.file_size() : 0);
        archive_entry_set_filetype(entry, file.is_directory() ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry, file.is_directory() ? 0755 : 0644);
        archive_write_header(a, entry);
        if (file.is_regular_file()) {
            std::ifstream ifs(file.path(), std::ios::binary);
            char buff[8192];
            while (ifs.read(buff, sizeof(buff))) {
                archive_write_data(a, buff, ifs.gcount());
            }
            archive_write_data(a, buff, ifs.gcount());
        }
        archive_entry_free(entry);
    }
    archive_write_close(a);
    archive_write_free(a);
}

std::map<std::string, std::string> getDependencies(const std::string& tgzFilePath) {
    std::string packageJsonContent;
    std::map<std::string, std::string> dependencies;
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, tgzFilePath.c_str(), 10240) != ARCHIVE_OK) return dependencies;
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (fs::path(archive_entry_pathname(entry)) == fs::path("package/package.json")) {
            size_t size = archive_entry_size(entry);
            packageJsonContent.resize(size);
            archive_read_data(a, &packageJsonContent[0], size);
            break;
        }
    }
    archive_read_free(a);
    if (!packageJsonContent.empty()) {
        try {
            auto json = nlohmann::json::parse(packageJsonContent);
            if (json.contains("dependencies")) {
                for (auto& [key, value] : json["dependencies"].items()) {
                    dependencies[key] = value.get<std::string>();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "JSON Parsing error in " << tgzFilePath << ": " << e.what() << std::endl;
        }
    }
    return dependencies;
}

void extractAndRepackage(const std::string& downloadedFile, const std::string& version) {
    const fs::path extractPath = "./extracted_graphics_tools";
    const fs::path packagePath = "./package";
    fs::create_directories(extractPath);
    extractArchive(downloadedFile, extractPath.string());
    const fs::path subfolderPath = extractPath / ("MixedReality-GraphicsTools-Unity-" + version) / "com.microsoft.mrtk.graphicstools.unity";
    if (fs::exists(subfolderPath)) {
        fs::rename(subfolderPath, packagePath);
    }
    const std::string tgzFileName = "com.microsoft.mrtk.graphicstools.unity-" + version + ".tgz";
    createTgzArchive(packagePath, tgzFileName);
    fs::remove_all(extractPath);
    fs::remove_all(packagePath);
    fs::remove(downloadedFile);
}

std::pair<std::string, std::string> extractComponentInfo(const std::string& fileName) {
    static const std::regex re("org\\.mixedrealitytoolkit\\.(.+?)-([0-9]+\\.[0-9]+\\.[0-9]+(?:-pre\\.[0-9]+)?)\\.tgz");
    std::smatch match;
    if (std::regex_search(fileName, match, re) && match.size() > 2) {
        return {match[1].str(), match[2].str()};
    }
    return {"", ""};
}

std::string findDownloadUrlForComponent(const std::string& component_name, const std::string& version, const nlohmann::json& jsonParsed) {
    for (const auto& release : jsonParsed) {
        if (release.contains("assets")) {
            for (const auto& asset : release["assets"]) {
                std::string fileName = asset.value("name", "");
                auto [name, ver] = extractComponentInfo(fileName);
                if (name == component_name && ver == version) {
                    return asset.value("browser_download_url", "");
                }
            }
        }
    }
    return "";
}

void downloadAndProcessDependencies(
    const std::string& component,
    const std::string& version,
    const nlohmann::json& jsonParsed,
    std::set<std::string>& processedComponents,
    std::map<std::string, std::string>& downloadedComponents,
    const std::map<std::string, std::function<void(const std::string&, const std::string&, std::map<std::string, std::string>&)>>& customHandlers) {

    const std::string componentKey = component + "-" + version;
    if (processedComponents.count(componentKey)) return;
    if (downloadedComponents.count(component) && !isNewerVersion(downloadedComponents[component], version)) return;
    std::string downloadUrl = findDownloadUrlForComponent(component, version, jsonParsed);
    if (downloadUrl.empty()) {
        std::cerr << "Component not found: " << component << " version " << version << std::endl;
        return;
    }
    std::cout << "Downloading " << component << " v" << version << "..." << std::endl;
    std::string downloadedFile = downloadFile(downloadUrl);
    if (downloadedFile.empty()) {
        std::cerr << "Failed to download " << component << std::endl;
        return;
    }
    downloadedComponents[component] = version;
    processedComponents.insert(componentKey);
    auto dependencies = getDependencies(downloadedFile);
    for (const auto& [depName, depVersion] : dependencies) {
        std::cout << "  Found dependency: " << depName << " v" << depVersion << std::endl;
        if (customHandlers.count(depName)) {
            customHandlers.at(depName)(depName, depVersion, downloadedComponents);
        } else if (depName.starts_with("org.mixedrealitytoolkit")) {
            std::string depComponent = depName.substr(24);
            downloadAndProcessDependencies(depComponent, depVersion, jsonParsed, processedComponents, downloadedComponents, customHandlers);
        }
    }
}

bool isValidUnityProject(const fs::path& path) {
    return fs::exists(path / "Assets") && fs::exists(path / "Packages") && fs::exists(path / "ProjectSettings");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: Please drag your Unity project folder onto the executable." << std::endl;
        return 1;
    }

    fs::path projectPath = argv[1];
    if (!fs::is_directory(projectPath) || !isValidUnityProject(projectPath)) {
        std::cout << "The provided path is not a valid Unity project folder." << std::endl;
        return 1;
    }

    std::string currentUnityVersionStr = getUnityVersion(projectPath);
    if (currentUnityVersionStr.empty()) {
        std::cerr << "Warning: Could not determine Unity editor version from ProjectSettings/ProjectVersion.txt" << std::endl;
    } else {
        std::cout << "Detected Unity Version: " << currentUnityVersionStr << std::endl;
    }

    std::cout << "Fetching MRTK release information from GitHub..." << std::endl;
    const std::string github_api_url = "https://api.github.com/repos/MixedRealityToolkit/MixedRealityToolkit-Unity/releases";
    std::string jsonResponse = httpGet(github_api_url);
    if (jsonResponse.empty()) {
        std::cerr << "Failed to get data from GitHub API." << std::endl;
        return 1;
    }

    try {
        auto jsonParsed = nlohmann::json::parse(jsonResponse);
        std::map<std::string, std::vector<std::string>> mrtk_components;
        std::vector<SelectablePackage> allPackages;

        for (const auto& release : jsonParsed) {
            for (const auto& asset : release["assets"]) {
                if (asset.value("name", "").ends_with(".tgz")) {
                    auto [name, version] = extractComponentInfo(asset["name"]);
                    if (!name.empty()) {
                        mrtk_components[name].push_back(version);
                    }
                }
            }
        }
        
        std::vector<std::string> mrtkNames;
        for (auto const& [name, versions] : mrtk_components) {
            mrtkNames.push_back(name);
        }
        std::sort(mrtkNames.begin(), mrtkNames.end());
        for(const auto& name : mrtkNames) {
            allPackages.push_back({name, name, PackageType::MRTK});
        }
        
        // Add hardcoded OpenXR packages
        allPackages.push_back({"Microsoft Mixed Reality OpenXR", "com.microsoft.mixedreality.openxr", PackageType::OpenXR});
        allPackages.push_back({"Meta OpenXR", "com.unity.xr.meta-openxr", PackageType::OpenXR});
        
        // Display Menu
        int currentIndex = 0;
        std::cout << "\nAvailable MRTK Components:\n------------------------\n";
        for (const auto& pkg : allPackages) {
            if (pkg.type == PackageType::MRTK) {
                std::cout << "[" << currentIndex++ << "] " << pkg.displayName << std::endl;
            }
        }
        std::cout << "\nOpenXR Components:\n------------------------\n";
        for (const auto& pkg : allPackages) {
            if (pkg.type == PackageType::OpenXR) {
                 std::cout << "[" << currentIndex++ << "] " << pkg.displayName << std::endl;
            }
        }

        std::cout << "\nEnter the index numbers of packages to install (e.g., 2 7 14):" << std::endl;
        std::string inputLine;
        std::getline(std::cin, inputLine);
        std::stringstream ss(inputLine);
        int inputIndex;
        std::vector<int> selectedIndices;
        while (ss >> inputIndex) {
            if (inputIndex >= 0 && inputIndex < allPackages.size()) {
                selectedIndices.push_back(inputIndex);
            } else {
                std::cout << "Invalid index: " << inputIndex << ". Skipping.\n";
            }
        }

        if (selectedIndices.empty()) {
            std::cout << "No packages selected. Exiting." << std::endl;
            return 0;
        }

        std::map<std::string, std::function<void(const std::string&, const std::string&, std::map<std::string, std::string>&)>> customHandlers;
        customHandlers["com.microsoft.mrtk.graphicstools.unity"] = 
            [](const std::string& name, const std::string& version, std::map<std::string, std::string>& downloaded) {
                const std::string tgzName = "com.microsoft.mrtk.graphicstools.unity-" + version + ".tgz";
                if (fs::exists(tgzName)) {
                    std::cout << "  Dependency already present: " << name << std::endl;
                    return;
                }
                std::cout << "  Handling special dependency: " << name << " v" << version << std::endl;
                const std::string url = "https://github.com/microsoft/MixedReality-GraphicsTools-Unity/archive/refs/tags/v" + version + ".tar.gz";
                std::string downloadedDep = downloadFile(url);
                if (!downloadedDep.empty()) {
                    downloaded[name] = version;
                    extractAndRepackage(downloadedDep, version);
                }
            };
        
        std::set<std::string> processedComponents;
        std::map<std::string, std::string> downloadedComponents;
        std::set<std::string> selectedOpenXRPackages;
        
        for (int idx : selectedIndices) {
            const auto& pkg = allPackages.at(idx);
            if (pkg.type == PackageType::MRTK) {
                auto& versions = mrtk_components.at(pkg.identifier);
                std::sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) {
                    return isNewerVersion(b, a);
                });
                const std::string& latestVersion = versions.front();
                std::cout << "\nProcessing " << pkg.displayName << " (latest: v" << latestVersion << ")" << std::endl;
                downloadAndProcessDependencies(pkg.identifier, latestVersion, jsonParsed, processedComponents, downloadedComponents, customHandlers);
            } else if (pkg.type == PackageType::OpenXR) {
                std::cout << "\nQueueing " << pkg.displayName << " for manifest update." << std::endl;
                selectedOpenXRPackages.insert(pkg.identifier);
            }
        }
        
        const fs::path mixedRealityDir = "MixedReality";
        fs::create_directories(mixedRealityDir);
        for (const auto& file : fs::directory_iterator(".")) {
            if (file.path().extension() == ".tgz") {
                fs::rename(file.path(), mixedRealityDir / file.path().filename());
            }
        }

        const fs::path destination = projectPath / "Packages" / mixedRealityDir;
        if (fs::exists(mixedRealityDir)) {
            if (fs::exists(destination)) {
                fs::remove_all(destination);
            }
            fs::rename(mixedRealityDir, destination);
        }

        fs::path manifestPath = projectPath / "Packages" / "manifest.json";
        if (!fs::exists(manifestPath)) {
            std::cerr << "manifest.json not found in Packages folder." << std::endl;
            return 1;
        }
        std::ifstream manifestFile(manifestPath);
        nlohmann::json manifestJson;
        manifestFile >> manifestJson;
        manifestFile.close();

        const fs::path installedMixedRealityDir = projectPath / "Packages" / "MixedReality";
        if (fs::exists(installedMixedRealityDir)) {
            for (const auto& file : fs::directory_iterator(installedMixedRealityDir)) {
                if (file.path().extension() == ".tgz") {
                    std::string filename = file.path().filename().string();
                    std::string componentName = filename.substr(0, filename.find_last_of('-'));
                    std::string dependencyPath = "file:MixedReality/" + filename;
                    manifestJson["dependencies"][componentName] = dependencyPath;
                }
            }
        }
        
        if (!selectedOpenXRPackages.empty()) {
             std::cout << "Adding selected OpenXR packages to manifest..." << std::endl;
        }

        if (selectedOpenXRPackages.count("com.microsoft.mixedreality.openxr")) {
            manifestJson["dependencies"]["com.microsoft.mixedreality.openxr"] = "1.11.2";
        }
        if (selectedOpenXRPackages.count("com.unity.xr.meta-openxr")) {
            if (!currentUnityVersionStr.empty()) {
                UnityVersion currentVersion(currentUnityVersionStr);
                if (currentVersion > UnityVersion("6000.0.0")) {
                    std::cout << "Unity 6+ detected. Adding Meta OpenXR v2.2.0." << std::endl;
                    manifestJson["dependencies"]["com.unity.xr.meta-openxr"] = "2.2.0";
                } else if (currentVersion > UnityVersion("2022.3.0f1")) {
                    std::cout << "Unity 2022.3+ detected. Adding Meta OpenXR v1.0.4." << std::endl;
                    manifestJson["dependencies"]["com.unity.xr.meta-openxr"] = "1.0.4";
                } else {
                    std::cout << "Older Unity version detected. Skipping Meta OpenXR package." << std::endl;
                }
            } else {
                std::cerr << "Warning: Could not add Meta OpenXR package because Unity version is unknown." << std::endl;
            }
        }

        std::ofstream outFile(manifestPath);
        outFile << manifestJson.dump(4);
        outFile.close();
        std::cout << "\nSuccessfully updated manifest.json in " << projectPath.string() << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "An unexpected error occurred: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}