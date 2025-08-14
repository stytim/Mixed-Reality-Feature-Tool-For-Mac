#include "core_logic.h"
#include <fstream>
#include <sstream>
#include <functional>
#include <algorithm>
#include <regex>
#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>

// ---- START: cURL CALLBACKS ----
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

static CURLcode performCurlRequest(const std::string& url, void* write_data, size_t (*write_function)(void*, size_t, size_t, void*)) {
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
// ---- END: cURL CALLBACKS ----

// ---- START: UnityVersion IMPLEMENTATION ----
UnityVersion::UnityVersion(const std::string& version_str) {
    if (version_str.empty()) return;
    std::sscanf(version_str.c_str(), "%d.%d.%d", &major, &minor, &patch);
    size_t type_pos = version_str.find_first_of("abfp");
    if (type_pos != std::string::npos) {
        type = version_str[type_pos];
        build = std::stoi(version_str.substr(type_pos + 1));
    }
}

bool UnityVersion::operator>(const UnityVersion& other) const {
    if (major != other.major) return major > other.major;
    if (minor != other.minor) return minor > other.minor;
    if (patch != other.patch) return patch > other.patch;
    if (type != other.type) return type > other.type; 
    return build > other.build;
}
// ---- END: UnityVersion IMPLEMENTATION ----

// ---- START: MRTKToolCore IMPLEMENTATION ----
MRTKToolCore::MRTKToolCore() = default;

bool MRTKToolCore::fetchAvailablePackages() {
    std::cout << "Fetching MRTK release information from GitHub..." << std::endl;
    const std::string github_api_url = "https://api.github.com/repos/MixedRealityToolkit/MixedRealityToolkit-Unity/releases";
    std::string jsonResponse = httpGet(github_api_url);
    if (jsonResponse.empty()) {
        std::cerr << "Failed to get data from GitHub API." << std::endl;
        return false;
    }

    try {
        githubJsonParsed = nlohmann::json::parse(jsonResponse);

        for (const auto& release : githubJsonParsed) {
            if (!release.contains("assets")) continue;
            for (const auto& asset : release["assets"]) {
                if (asset.value("name", "").ends_with(".tgz")) {
                    auto [name, version] = extractComponentInfo(asset["name"]);
                    if (!name.empty()) {
                        mrtkComponentVersions[name].push_back(version);
                    }
                }
            }
        }
        
        std::vector<std::string> mrtkNames;
        for (auto const& [name, versions] : mrtkComponentVersions) {
            mrtkNames.push_back(name);
        }
        std::sort(mrtkNames.begin(), mrtkNames.end());
        for(const auto& name : mrtkNames) {
            allPackages.push_back({name, name, PackageType::MRTK});
        }
        
        allPackages.push_back({"Microsoft Mixed Reality OpenXR", "com.microsoft.mixedreality.openxr", PackageType::OpenXR});
        allPackages.push_back({"Meta OpenXR", "com.unity.xr.meta-openxr", PackageType::OpenXR});

    } catch(const std::exception& e) {
        std::cerr << "An error occurred during JSON parsing: " << e.what() << std::endl;
        return false;
    }
    return true;
}

const std::vector<SelectablePackage>& MRTKToolCore::getAvailablePackages() const {
    return allPackages;
}

void MRTKToolCore::resolveDependencies(const std::vector<int>& selectedIndices) {
    std::cout << "\n--- Phase 1: Resolving all dependencies... ---\n";
    std::set<std::string> processedComponents;

    // <<< NEW: Clear previous results
    requiredMrtkPackages.clear();
    resolvedUserSelections.clear();
    resolvedDependencies.clear();

    // First, identify the packages the user explicitly selected
    for (int idx : selectedIndices) {
        const auto& pkg = allPackages.at(idx);
        if (pkg.type == PackageType::MRTK) {
            auto& versions = mrtkComponentVersions.at(pkg.identifier);
            std::sort(versions.begin(), versions.end(), isNewerVersion);
            const std::string& latestVersion = versions.back();
            resolvedUserSelections[pkg.identifier] = latestVersion;
        } else if (pkg.type == PackageType::OpenXR) {
            requiredOpenXrPackages.insert(pkg.identifier);
        }
    }

    // Now, resolve dependencies for all user-selected packages
    for (const auto& [name, version] : resolvedUserSelections) {
         std::cout << "Processing selected package: " << name << " (latest: v" << version << ")" << std::endl;
         resolveDependenciesRecursive(name, version, processedComponents);
    }
    
    // <<< NEW: Differentiate dependencies from direct selections
    for (const auto& [name, version] : requiredMrtkPackages) {
        if (resolvedUserSelections.find(name) == resolvedUserSelections.end()) {
            resolvedDependencies[name] = version;
        }
    }
}

void MRTKToolCore::downloadAndRepackage() {
    std::cout << "\n--- Phase 2: Downloading required packages... ---\n";
    for (const auto& [name, version] : requiredMrtkPackages) {
         if (name == "com.microsoft.mrtk.graphicstools.unity") {
            std::cout << "Downloading and repackaging special dependency: " << name << " v" << version << "..." << std::endl;
            const std::string url = "https://github.com/microsoft/MixedReality-GraphicsTools-Unity/archive/refs/tags/v" + version + ".tar.gz";
            std::string downloadedDep = downloadFile(url);
            if (!downloadedDep.empty()) {
                extractAndRepackageGraphicsTools(downloadedDep, version);
            }
        } else {
            std::cout << "Downloading " << name << " v" << version << "..." << std::endl;
            std::string downloadUrl = findDownloadUrlForComponent(name, version);
            if (!downloadUrl.empty()) {
                downloadFile(downloadUrl);
            } else {
                std::cerr << "ERROR: Could not find final download URL for " << name << " v" << version << ". Skipping." << std::endl;
            }
        }
    }
}

void MRTKToolCore::installPackagesToProject(const fs::path& projectPath) {
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
        std::cout << "\nMoved packages to " << destination << std::endl;
    }

    fs::path manifestPath = projectPath / "Packages" / "manifest.json";
    if (!fs::exists(manifestPath)) {
        std::cerr << "manifest.json not found in Packages folder." << std::endl;
        return;
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
                // Assumes format like: org.mixedrealitytoolkit.core-3.0.0.tgz
                std::string componentName = filename.substr(0, filename.find_last_of('-'));
                std::string dependencyPath = "file:MixedReality/" + filename;
                manifestJson["dependencies"][componentName] = dependencyPath;
            }
        }
    }
    
    if (!requiredOpenXrPackages.empty()) {
         std::cout << "Adding selected OpenXR packages to manifest..." << std::endl;
    }

    if (requiredOpenXrPackages.count("com.microsoft.mixedreality.openxr")) {
        manifestJson["dependencies"]["com.microsoft.mixedreality.openxr"] = "1.11.2"; // Example version
    }
    if (requiredOpenXrPackages.count("com.unity.xr.meta-openxr")) {
        std::string currentUnityVersionStr = getUnityVersion(projectPath);
        if (!currentUnityVersionStr.empty()) {
            UnityVersion currentVersion(currentUnityVersionStr);
             if (currentVersion > UnityVersion("6000.0.0")) { // Unity 6 or newer
                std::cout << "Unity 6+ detected. Adding Meta OpenXR v2.2.0." << std::endl;
                manifestJson["dependencies"]["com.unity.xr.meta-openxr"] = "2.2.0";
            } else if (currentVersion > UnityVersion("2022.3.0f1")) { // Unity 2022.3 or newer
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
    std::cout << "Successfully updated manifest.json in " << projectPath.string() << std::endl;
}

bool MRTKToolCore::isValidUnityProject(const fs::path& path) {
    return fs::exists(path / "Assets") && fs::exists(path / "Packages") && fs::exists(path / "ProjectSettings");
}

std::string MRTKToolCore::getUnityVersion(const fs::path& projectPath) {
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

std::string MRTKToolCore::httpGet(const std::string& url) {
    std::string readBuffer;
    performCurlRequest(url, &readBuffer, WriteStringCallback);
    return readBuffer;
}

std::string MRTKToolCore::downloadFile(const std::string& url, const std::string& outputPath) {
    std::string filename = outputPath.empty() ? fs::path(url).filename().string() : outputPath;
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

void MRTKToolCore::extractArchive(const std::string& archiveFile, const std::string& extractPath) {
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
            if (!outputFile) continue;
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

void MRTKToolCore::createTgzArchive(const fs::path& folderPath, const fs::path& tgzFileName) {
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

std::map<std::string, std::string> MRTKToolCore::getDependenciesFromTgz(const std::string& tgzFilePath) {
    std::string packageJsonContent;
    std::map<std::string, std::string> dependencies;
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, tgzFilePath.c_str(), 10240) != ARCHIVE_OK) return dependencies;
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (fs::path(archive_entry_pathname(entry)).filename() == "package.json") {
            size_t size = archive_entry_size(entry);
            if (size > 0) {
                packageJsonContent.resize(size);
                archive_read_data(a, &packageJsonContent[0], size);
            }
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

void MRTKToolCore::extractAndRepackageGraphicsTools(const std::string& downloadedFile, const std::string& version) {
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

std::pair<std::string, std::string> MRTKToolCore::extractComponentInfo(const std::string& fileName) {
    static const std::regex re("org\\.mixedrealitytoolkit\\.(.+?)-([0-9]+\\.[0-9]+\\.[0-9]+(?:-pre\\.[0-9]+)?)\\.tgz");
    std::smatch match;
    if (std::regex_search(fileName, match, re) && match.size() > 2) {
        return {match[1].str(), match[2].str()};
    }
    return {"", ""};
}

std::string MRTKToolCore::findDownloadUrlForComponent(const std::string& component_name, const std::string& version) {
    for (const auto& release : githubJsonParsed) {
        if (!release.contains("assets")) continue;
        for (const auto& asset : release["assets"]) {
            std::string fileName = asset.value("name", "");
            auto [name, ver] = extractComponentInfo(fileName);
            if (name == component_name && ver == version) {
                return asset.value("browser_download_url", "");
            }
        }
    }
    return "";
}

void MRTKToolCore::resolveDependenciesRecursive(
    const std::string& component,
    const std::string& version,
    std::set<std::string>& processedComponents) {

    const std::string componentKey = component + "-" + version;

    if (processedComponents.count(componentKey)) return;

    if (requiredMrtkPackages.count(component) && !isNewerVersion(requiredMrtkPackages[component], version)) {
        return;
    }
    
    std::cout << "  Resolving " << component << " v" << version << std::endl;
    requiredMrtkPackages[component] = version;
    processedComponents.insert(componentKey);

    std::string downloadUrl = findDownloadUrlForComponent(component, version);
    if (downloadUrl.empty()) {
        if (component != "com.microsoft.mrtk.graphicstools.unity") {
            // This component (like graphics tools) doesn't have a direct download URL in the releases
        }
        return;
    }

    std::string tempFile = downloadFile(downloadUrl, "temp_dependency_check.tgz");
    if (tempFile.empty()) {
        std::cerr << "  Failed to download " << component << " for dependency check." << std::endl;
        return;
    }

    auto dependencies = getDependenciesFromTgz(tempFile);
    fs::remove(tempFile);

    for (const auto& [depName, depVersion] : dependencies) {
        if (depName.starts_with("com.unity.")) continue;

        if (depName.starts_with("org.mixedrealitytoolkit")) {
            std::string depComponent = depName.substr(24);
            resolveDependenciesRecursive(depComponent, depVersion, processedComponents);
        } else {
            resolveDependenciesRecursive(depName, depVersion, processedComponents);
        }
    }
}

bool MRTKToolCore::isNewerVersion(const std::string& v_old, const std::string& v_new) {
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

    bool old_is_pre = (old_pre_pos != std::string::npos);
    bool new_is_pre = (new_pre_pos != std::string::npos);

    if (old_is_pre && !new_is_pre) return true;
    if (!old_is_pre && new_is_pre) return false;
    if (!old_is_pre && !new_is_pre) return false;

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
// ---- END: MRTKToolCore IMPLEMENTATION ----