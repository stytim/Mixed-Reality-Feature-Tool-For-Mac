#include "core_logic.h"
#include <fstream>
#include <sstream>
#include <functional>
#include <algorithm>
#include <random>
#include <regex>
#include <system_error>
#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>

// ---- START: cURL CALLBACKS ----
static size_t WriteDataCallback(void* buffer, size_t size, size_t nmemb, void* userp) {
    auto* outputFile = static_cast<std::ofstream*>(userp);
    size_t actualSize = size * nmemb;
    outputFile->write(static_cast<char*>(buffer), actualSize);
    return outputFile->good() ? actualSize : 0;
}

static size_t WriteStringCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* s = static_cast<std::string*>(userp);
    size_t newLength = size * nmemb;
    try {
        s->append(static_cast<char*>(contents), newLength);
        return newLength;
    } catch (std::bad_alloc&) {
        return 0;
    }
}

static CURLcode performCurlRequest(const std::string& url, void* write_data, size_t (*write_function)(void*, size_t, size_t, void*)) {
    CURL* curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);   // Treat 4xx/5xx as transfer failure.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);      // Safe under threaded callers.
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

// ---- START: libarchive RAII wrappers ----
namespace {
struct ArchiveReader {
    struct archive* a;
    ArchiveReader() : a(archive_read_new()) {}
    ~ArchiveReader() { if (a) archive_read_free(a); }
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;
};

struct ArchiveWriter {
    struct archive* a;
    ArchiveWriter() : a(archive_write_new()) {}
    ~ArchiveWriter() {
        if (a) {
            archive_write_close(a);
            archive_write_free(a);
        }
    }
    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;
};
} // namespace
// ---- END: libarchive RAII wrappers ----

// ---- START: shared filename parser ----
// Matches "<package>-<version>.tgz" where version is N.N.N optionally followed by -pre.N.
// Captures the package identifier (group 1) and version (group 2).
std::pair<std::string, std::string> parsePackageNameFromTgz(const std::string& fileName) {
    static const std::regex re(R"(^(.+?)-([0-9]+\.[0-9]+\.[0-9]+(?:-pre\.[0-9]+)?)\.tgz$)");
    std::smatch match;
    if (std::regex_match(fileName, match, re) && match.size() > 2) {
        return {match[1].str(), match[2].str()};
    }
    return {"", ""};
}
// ---- END: shared filename parser ----

// ---- START: UnityVersion IMPLEMENTATION ----
UnityVersion::UnityVersion(const std::string& version_str) {
    if (version_str.empty()) return;
    // sscanf leaves uninitialized fields untouched; our defaults are zero-initialized in the header.
    std::sscanf(version_str.c_str(), "%d.%d.%d", &major, &minor, &patch);
    size_t type_pos = version_str.find_first_of("abfp");
    if (type_pos != std::string::npos && type_pos + 1 < version_str.size()) {
        type = version_str[type_pos];
        try {
            build = std::stoi(version_str.substr(type_pos + 1));
        } catch (const std::exception&) {
            build = 0;
        }
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
MRTKToolCore::MRTKToolCore() {
    static std::atomic<uint64_t> counter{0};
    std::random_device rd;
    auto unique = std::to_string(rd()) + "-"
                + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    std::error_code ec;
    workDir = fs::temp_directory_path(ec) / ("mrtk-" + unique);
    if (ec) workDir = fs::path("mrtk-" + unique);
    fs::create_directories(workDir, ec);
}

void MRTKToolCore::setProgressCallback(ProgressCallback cb) {
    progressCallback = std::move(cb);
}

void MRTKToolCore::log(const std::string& message) const {
    if (progressCallback) progressCallback(message);
    else std::cout << message << std::endl;
}

bool MRTKToolCore::fetchAvailablePackages() {
    // Clear state so re-discovery (Start Over → Discover again) produces a fresh list.
    allPackages.clear();
    mrtkComponentVersions.clear();
    githubJsonParsed = nlohmann::json{};

    log("Fetching MRTK release information from GitHub...");
    const std::string github_api_url = "https://api.github.com/repos/MixedRealityToolkit/MixedRealityToolkit-Unity/releases";
    std::string jsonResponse = httpGet(github_api_url);
    if (jsonResponse.empty()) {
        log("Failed to get data from GitHub API.");
        return false;
    }

    try {
        githubJsonParsed = nlohmann::json::parse(jsonResponse);

        for (const auto& release : githubJsonParsed) {
            if (!release.contains("assets")) continue;
            for (const auto& asset : release["assets"]) {
                if (asset.value("name", "").ends_with(".tgz")) {
                    auto [name, version] = parsePackageNameFromTgz(asset["name"]);
                    if (name.starts_with("org.mixedrealitytoolkit.")) {
                        // The display name in the menu is the suffix after the prefix.
                        std::string suffix = name.substr(std::string("org.mixedrealitytoolkit.").size());
                        if (!suffix.empty()) {
                            mrtkComponentVersions[suffix].push_back(version);
                        }
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
        log(std::string("An error occurred during JSON parsing: ") + e.what());
        return false;
    }
    return true;
}

const std::vector<SelectablePackage>& MRTKToolCore::getAvailablePackages() const {
    return allPackages;
}

void MRTKToolCore::resolveDependencies(const std::vector<int>& selectedIndices) {
    log("\n--- Phase 1: Resolving all dependencies... ---");
    std::set<std::string> processedComponents;

    requiredMrtkPackages.clear();
    resolvedUserSelections.clear();
    resolvedDependencies.clear();
    requiredOpenXrPackages.clear();
    downloadedFiles.clear();

    for (int idx : selectedIndices) {
        if (idx < 0 || static_cast<size_t>(idx) >= allPackages.size()) continue;
        const auto& pkg = allPackages.at(idx);
        if (pkg.type == PackageType::MRTK) {
            auto& versions = mrtkComponentVersions.at(pkg.identifier);
            std::sort(versions.begin(), versions.end(), isNewerVersion);
            const std::string& latestVersion = versions.back();
            resolvedUserSelections[pkg.identifier] = latestVersion;
        } else if (pkg.type == PackageType::OpenXR) {
            requiredOpenXrPackages.insert(pkg.identifier);
            // Also show in the UI summary panel.
            resolvedUserSelections[pkg.displayName] = "(OpenXR Runtime)";
        }
    }

    for (const auto& [name, version] : resolvedUserSelections) {
        if (version == "(OpenXR Runtime)") continue;
        log("Processing selected package: " + name + " (latest: v" + version + ")");
        resolveDependenciesRecursive(name, version, processedComponents);
    }

    for (const auto& [name, version] : requiredMrtkPackages) {
        if (resolvedUserSelections.find(name) == resolvedUserSelections.end()) {
            resolvedDependencies[name] = version;
        }
    }
}

void MRTKToolCore::downloadAndRepackage() {
    log("\n--- Phase 2: Downloading required packages... ---");
    downloadedFiles.clear();
    for (const auto& [name, version] : requiredMrtkPackages) {
         if (name == "com.microsoft.mrtk.graphicstools.unity") {
            log("Downloading and repackaging special dependency: " + name + " v" + version + "...");
            const std::string url = "https://github.com/microsoft/MixedReality-GraphicsTools-Unity/archive/refs/tags/v" + version + ".tar.gz";
            std::string downloadedDep = downloadFile(url);
            if (!downloadedDep.empty()) {
                extractAndRepackageGraphicsTools(downloadedDep, version);
            }
        } else {
            log("Downloading " + name + " v" + version + "...");
            std::string downloadUrl = findDownloadUrlForComponent(name, version);
            if (!downloadUrl.empty()) {
                std::string out = downloadFile(downloadUrl);
                if (!out.empty()) downloadedFiles.push_back(fs::path(out));
            } else {
                log("ERROR: Could not find final download URL for " + name + " v" + version + ". Skipping.");
            }
        }
    }
}

void MRTKToolCore::installPackagesToProject(const fs::path& projectPath) {
    const fs::path destination = projectPath / "Packages" / "MixedReality";
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        log("Error: Could not create destination folder " + destination.string() + ": " + ec.message());
        return;
    }

    // Track which files actually made it into the project so manifest updates only reference those.
    std::vector<fs::path> installedFiles;
    for (const auto& src : downloadedFiles) {
        if (src.extension() != ".tgz") continue;
        fs::path dst = destination / src.filename();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            log("Error: Failed to copy " + src.filename().string() + ": " + ec.message());
            ec.clear();
            continue;
        }
        installedFiles.push_back(dst);
        fs::remove(src, ec);
        ec.clear();
    }

    if (!installedFiles.empty()) {
        log("\nCopied " + std::to_string(installedFiles.size()) + " package(s) to " + destination.string());
    }

    fs::path manifestPath = projectPath / "Packages" / "manifest.json";
    if (!fs::exists(manifestPath)) {
        log("manifest.json not found in Packages folder.");
        return;
    }
    nlohmann::json manifestJson;
    try {
        std::ifstream manifestFile(manifestPath);
        manifestFile >> manifestJson;
    } catch (const std::exception& e) {
        log(std::string("Failed to parse manifest.json: ") + e.what());
        return;
    }
    if (!manifestJson.contains("dependencies") || !manifestJson["dependencies"].is_object()) {
        manifestJson["dependencies"] = nlohmann::json::object();
    }

    for (const auto& file : installedFiles) {
        std::string filename = file.filename().string();
        auto [componentName, version] = parsePackageNameFromTgz(filename);
        if (componentName.empty()) {
            log("Skipping manifest entry (could not parse name): " + filename);
            continue;
        }
        manifestJson["dependencies"][componentName] = "file:MixedReality/" + filename;
    }

    if (!requiredOpenXrPackages.empty()) {
         log("Adding selected OpenXR packages to manifest...");
    }

    if (requiredOpenXrPackages.count("com.microsoft.mixedreality.openxr")) {
        manifestJson["dependencies"]["com.microsoft.mixedreality.openxr"] = "1.11.2";
    }
    if (requiredOpenXrPackages.count("com.unity.xr.meta-openxr")) {
        std::string currentUnityVersionStr = getUnityVersion(projectPath);
        if (!currentUnityVersionStr.empty()) {
            UnityVersion currentVersion(currentUnityVersionStr);
            if (currentVersion > UnityVersion("6000.0.0")) {
                log("Unity 6+ detected. Adding Meta OpenXR v2.2.0.");
                manifestJson["dependencies"]["com.unity.xr.meta-openxr"] = "2.2.0";
            } else if (currentVersion > UnityVersion("2022.3.0f1")) {
                log("Unity 2022.3+ detected. Adding Meta OpenXR v1.0.4.");
                manifestJson["dependencies"]["com.unity.xr.meta-openxr"] = "1.0.4";
            } else {
                log("Older Unity version detected. Skipping Meta OpenXR package.");
            }
        } else {
            log("Warning: Could not add Meta OpenXR package because Unity version is unknown.");
        }
    }

    try {
        std::ofstream outFile(manifestPath);
        outFile << manifestJson.dump(4);
    } catch (const std::exception& e) {
        log(std::string("Failed to write manifest.json: ") + e.what());
        return;
    }
    log("Successfully updated manifest.json in " + projectPath.string());

    // Best-effort cleanup of the per-instance work directory.
    fs::remove_all(workDir, ec);
}

bool MRTKToolCore::isValidUnityProject(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path / "Assets", ec) && fs::exists(path / "Packages", ec) && fs::exists(path / "ProjectSettings", ec);
}

std::string MRTKToolCore::getUnityVersion(const fs::path& projectPath) {
    fs::path version_file = projectPath / "ProjectSettings" / "ProjectVersion.txt";
    std::error_code ec;
    if (!fs::exists(version_file, ec)) return "";
    std::ifstream file(version_file);
    if (!file) return "";
    std::string line;
    static const std::regex re(R"(m_EditorVersion:\s*(.+))");
    while (std::getline(file, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re) && m.size() > 1) {
            std::string value = m[1].str();
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            return value;
        }
    }
    return "";
}

std::string MRTKToolCore::httpGet(const std::string& url) {
    std::string readBuffer;
    performCurlRequest(url, &readBuffer, WriteStringCallback);
    return readBuffer;
}

std::string MRTKToolCore::downloadFile(const std::string& url, const fs::path& outputPath) {
    fs::path target = outputPath;
    if (target.empty()) {
        std::string fname = fs::path(url).filename().string();
        if (fname.empty()) {
            log("Error: Could not extract file name from URL.");
            return "";
        }
        target = workDir / fname;
    }
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream outputFile(target, std::ios::binary);
    if (!outputFile) {
        log("Error: Cannot open file " + target.string() + " for writing.");
        return "";
    }
    if (performCurlRequest(url, &outputFile, WriteDataCallback) != CURLE_OK) {
        outputFile.close();
        fs::remove(target, ec);
        return "";
    }
    return target.string();
}

bool MRTKToolCore::extractArchive(const fs::path& archiveFile, const fs::path& extractPath) {
    ArchiveReader reader;
    if (!reader.a) return false;
    archive_read_support_format_all(reader.a);
    archive_read_support_filter_all(reader.a);
    if (archive_read_open_filename(reader.a, archiveFile.string().c_str(), 10240) != ARCHIVE_OK) {
        log(std::string("Error opening archive: ") + archive_error_string(reader.a));
        return false;
    }

    std::error_code ec;
    fs::create_directories(extractPath, ec);
    fs::path canonicalBase = fs::weakly_canonical(extractPath, ec);
    if (ec) canonicalBase = extractPath.lexically_normal();

    struct archive_entry* entry = nullptr;
    while (true) {
        int r = archive_read_next_header(reader.a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN) {
            log(std::string("Archive read error: ") + archive_error_string(reader.a));
            return false;
        }

        fs::path entryPath = archive_entry_pathname(entry);
        fs::path fullPath = (extractPath / entryPath).lexically_normal();
        fs::path canonicalFull = fs::weakly_canonical(fullPath, ec);
        if (ec) canonicalFull = fullPath;
        // Reject entries that escape the extraction root (CVE-2007-4559 / Zip Slip).
        auto rel = std::filesystem::relative(canonicalFull, canonicalBase, ec);
        if (ec || rel.empty() || rel.string().rfind("..", 0) == 0) {
            log("Skipping unsafe archive entry: " + entryPath.string());
            ec.clear();
            continue;
        }

        fs::create_directories(fullPath.parent_path(), ec);
        if (archive_entry_filetype(entry) == AE_IFREG) {
            std::ofstream outputFile(fullPath, std::ios::binary);
            if (!outputFile) continue;
            const void* buff;
            size_t size;
            la_int64_t offset;
            while (true) {
                int rd = archive_read_data_block(reader.a, &buff, &size, &offset);
                if (rd == ARCHIVE_EOF) break;
                if (rd < ARCHIVE_OK) {
                    log(std::string("Archive read_data error: ") + archive_error_string(reader.a));
                    break;
                }
                outputFile.write(static_cast<const char*>(buff), size);
                if (!outputFile) {
                    log("Write failed extracting " + fullPath.string());
                    break;
                }
            }
        }
    }
    return true;
}

bool MRTKToolCore::createTgzArchive(const fs::path& folderPath, const fs::path& tgzFileName) {
    ArchiveWriter writer;
    if (!writer.a) return false;
    if (archive_write_add_filter_gzip(writer.a) != ARCHIVE_OK ||
        archive_write_set_format_pax_restricted(writer.a) != ARCHIVE_OK ||
        archive_write_open_filename(writer.a, tgzFileName.string().c_str()) != ARCHIVE_OK) {
        log(std::string("Failed to open output archive: ") + archive_error_string(writer.a));
        return false;
    }

    std::string dirName = folderPath.filename().string();
    std::error_code ec;
    for (const auto& file : fs::recursive_directory_iterator(folderPath, ec)) {
        const std::string relativePath = fs::relative(file.path(), folderPath, ec).string();
        if (ec) { ec.clear(); continue; }
        fs::path archivePath = fs::path(dirName) / relativePath;

        struct archive_entry* entry = archive_entry_new();
        if (!entry) continue;

        bool isDir = file.is_directory(ec);
        bool isFile = !ec && file.is_regular_file(ec);
        ec.clear();

        archive_entry_set_pathname(entry, archivePath.string().c_str());
        if (isFile) {
            uintmax_t sz = file.file_size(ec);
            archive_entry_set_size(entry, ec ? 0 : static_cast<la_int64_t>(sz));
            ec.clear();
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
        } else if (isDir) {
            archive_entry_set_size(entry, 0);
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
        } else {
            archive_entry_free(entry);
            continue;
        }

        if (archive_write_header(writer.a, entry) != ARCHIVE_OK) {
            log(std::string("write_header failed: ") + archive_error_string(writer.a));
            archive_entry_free(entry);
            continue;
        }

        if (isFile) {
            std::ifstream ifs(file.path(), std::ios::binary);
            char buff[8192];
            while (ifs) {
                ifs.read(buff, sizeof(buff));
                auto n = ifs.gcount();
                if (n <= 0) break;
                la_ssize_t written = archive_write_data(writer.a, buff, static_cast<size_t>(n));
                if (written < 0) {
                    log(std::string("write_data failed: ") + archive_error_string(writer.a));
                    break;
                }
            }
        }
        archive_entry_free(entry);
    }
    return true;
}

std::map<std::string, std::string> MRTKToolCore::getDependenciesFromTgz(const fs::path& tgzFilePath) {
    std::string packageJsonContent;
    std::map<std::string, std::string> dependencies;

    ArchiveReader reader;
    if (!reader.a) return dependencies;
    archive_read_support_filter_all(reader.a);
    archive_read_support_format_all(reader.a);
    if (archive_read_open_filename(reader.a, tgzFilePath.string().c_str(), 10240) != ARCHIVE_OK) {
        return dependencies;
    }

    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(reader.a, &entry) == ARCHIVE_OK) {
        if (fs::path(archive_entry_pathname(entry)).filename() == "package.json") {
            la_int64_t size = archive_entry_size(entry);
            if (size > 0) {
                packageJsonContent.resize(static_cast<size_t>(size));
                la_ssize_t read = archive_read_data(reader.a, &packageJsonContent[0], static_cast<size_t>(size));
                if (read < 0) packageJsonContent.clear();
                else if (static_cast<size_t>(read) < packageJsonContent.size()) packageJsonContent.resize(static_cast<size_t>(read));
            }
            break;
        }
    }

    if (!packageJsonContent.empty()) {
        try {
            auto json = nlohmann::json::parse(packageJsonContent);
            if (json.contains("dependencies")) {
                for (auto& [key, value] : json["dependencies"].items()) {
                    dependencies[key] = value.get<std::string>();
                }
            }
        } catch (const std::exception& e) {
            log("JSON Parsing error in " + tgzFilePath.string() + ": " + e.what());
        }
    }
    return dependencies;
}

void MRTKToolCore::extractAndRepackageGraphicsTools(const fs::path& downloadedFile, const std::string& version) {
    std::error_code ec;
    fs::path extractPath = workDir / ("extracted_graphics_tools_" + version);
    fs::path packagePath = workDir / ("package_" + version);
    fs::remove_all(extractPath, ec);
    fs::remove_all(packagePath, ec);
    fs::create_directories(extractPath, ec);

    if (!extractArchive(downloadedFile, extractPath)) {
        log("Failed to extract graphics tools archive.");
        fs::remove_all(extractPath, ec);
        fs::remove(downloadedFile, ec);
        return;
    }

    fs::path subfolderPath = extractPath / ("MixedReality-GraphicsTools-Unity-" + version) / "com.microsoft.mrtk.graphicstools.unity";
    if (fs::exists(subfolderPath, ec)) {
        fs::rename(subfolderPath, packagePath, ec);
        if (ec) {
            // Cross-volume rename fails on some macOS setups; fall back to copy.
            ec.clear();
            fs::copy(subfolderPath, packagePath, fs::copy_options::recursive, ec);
        }
    }
    fs::path tgzFileName = workDir / ("com.microsoft.mrtk.graphicstools.unity-" + version + ".tgz");
    if (createTgzArchive(packagePath, tgzFileName)) {
        downloadedFiles.push_back(tgzFileName);
    }

    fs::remove_all(extractPath, ec);
    fs::remove_all(packagePath, ec);
    fs::remove(downloadedFile, ec);
}

std::string MRTKToolCore::findDownloadUrlForComponent(const std::string& component_name, const std::string& version) {
    // Asset names are the full identifier, e.g. "org.mixedrealitytoolkit.core-3.0.0.tgz",
    // but we resolve by the short suffix ("core") to match the user-facing menu.
    for (const auto& release : githubJsonParsed) {
        if (!release.contains("assets")) continue;
        for (const auto& asset : release["assets"]) {
            std::string fileName = asset.value("name", "");
            auto [name, ver] = parsePackageNameFromTgz(fileName);
            if (name.empty()) continue;
            if (name == component_name && ver == version) {
                return asset.value("browser_download_url", "");
            }
            if (name.starts_with("org.mixedrealitytoolkit.")) {
                std::string suffix = name.substr(std::string("org.mixedrealitytoolkit.").size());
                if (suffix == component_name && ver == version) {
                    return asset.value("browser_download_url", "");
                }
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

    log("  Resolving " + component + " v" + version);
    requiredMrtkPackages[component] = version;
    processedComponents.insert(componentKey);

    std::string downloadUrl = findDownloadUrlForComponent(component, version);
    if (downloadUrl.empty()) {
        // Some components (graphics tools) have no direct release asset; handled later.
        return;
    }

    fs::path tempFile = workDir / ("dep_check_" + component + "_" + version + ".tgz");
    std::string tempStr = downloadFile(downloadUrl, tempFile);
    if (tempStr.empty()) {
        log("  Failed to download " + component + " for dependency check.");
        return;
    }

    auto dependencies = getDependenciesFromTgz(tempFile);
    std::error_code ec;
    fs::remove(tempFile, ec);

    for (const auto& [depName, depVersion] : dependencies) {
        if (depName.starts_with("com.unity.")) continue;

        if (depName.starts_with("org.mixedrealitytoolkit")) {
            std::string depComponent = depName.substr(std::string("org.mixedrealitytoolkit.").size());
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

    auto safeStoi = [](const std::string& s) -> int {
        try { return std::stoi(s); } catch (const std::exception&) { return 0; }
    };

    std::stringstream ss_old(old_main);
    while (getline(ss_old, temp, '.')) { old_parts.push_back(safeStoi(temp)); }

    std::stringstream ss_new(new_main);
    while (getline(ss_new, temp, '.')) { new_parts.push_back(safeStoi(temp)); }

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
            int old_num = safeStoi(old_tag_parts[i]);
            int new_num = safeStoi(new_tag_parts[i]);
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
