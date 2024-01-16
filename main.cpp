#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <curl/curl.h>
#include "nlohmann/json.hpp"
#include <regex>
#include <map>
#include <set>
#include <queue>
#include <vector>
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fcntl.h> 
#include <algorithm> // For sorting and finding the latest version

std::map<std::string, std::string> downloadedComponents;
std::vector<std::string> componentNames;

bool isNewerVersion(const std::string &existingVersion, const std::string &newVersion) {
    // Split the version strings and compare each part
    std::istringstream existingStream(existingVersion);
    std::istringstream newStream(newVersion);
    std::string existingPart, newPart;

    while (std::getline(existingStream, existingPart, '.') && std::getline(newStream, newPart, '.')) {
        int existingNum = std::stoi(existingPart);
        int newNum = std::stoi(newPart);
        if (newNum > existingNum) return true;
        if (newNum < existingNum) return false;
    }
    return false;
}

// Callback function to write data received by libcurl
static size_t WriteDataCallback(void *buffer, size_t size, size_t nmemb, void *userp) {
    std::ofstream *outputFile = static_cast<std::ofstream*>(userp);
    size_t actualSize = size * nmemb;
    outputFile->write(static_cast<char*>(buffer), actualSize);
    return actualSize;
}

std::string downloadFile(const std::string &url) {
    CURL *curl;
    CURLcode res;
    std::string filename;

    // Extract file name from URL
    size_t pos = url.find_last_of('/');
    if (pos != std::string::npos) {
        filename = url.substr(pos + 1);
    } else {
        std::cerr << "Error: Could not extract file name from URL." << std::endl;
        return "";
    }

    curl = curl_easy_init();
    if(curl) {
        std::ofstream outputFile(filename, std::ios::binary);

        if (!outputFile) {
            std::cerr << "Error: Cannot open file " << filename << " for writing." << std::endl;
            return "";
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDataCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outputFile);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects

        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }

        outputFile.close();
        curl_easy_cleanup(curl);
    } else {
        std::cerr << "Error initializing curl." << std::endl;
    }
    return filename; // Return the file name
}

std::map<std::string, std::string> getDependencies(const std::string &url, const std::string &component, const std::string &version) {
    std::string packageJsonContent;
    std::map<std::string, std::string> dependencies;

    std::string tgzFilePath = downloadFile(url); // Get the file name from downloadFile function
    if (tgzFilePath.empty()) {
        std::cerr << "Error: Failed to download file." << std::endl;
        return std::map<std::string, std::string>();
    }

    // Use libarchive to extract package.json from the .tgz file
    struct archive *a;
    struct archive_entry *entry;
    int r;

    a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    r = archive_read_open_filename(a, tgzFilePath.c_str(), 10240); // Block size
    if (r != ARCHIVE_OK) {
        std::cerr << "Error opening archive: " << archive_error_string(a) << std::endl;
        return dependencies; // Return empty dependencies map
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string filePath = archive_entry_pathname(entry);
        if (filePath == "package/package.json") {
            // Read the content of package.json
            size_t size = archive_entry_size(entry);
            packageJsonContent.resize(size);
            archive_read_data(a, &packageJsonContent[0], size);
            break;
        }
    }
    archive_read_free(a);

    // Use nlohmann/json to parse packageJsonContent and extract dependencies
    if (!packageJsonContent.empty()) {
        try {
            auto json = nlohmann::json::parse(packageJsonContent);
            for (auto& [key, value] : json["dependencies"].items()) {
                dependencies[key] = value.get<std::string>();
            }
        } catch (const std::exception& e) {
            std::cerr << "JSON Parsing error: " << e.what() << std::endl;
        }
    }

    return dependencies;
}

// Callback function for writing data received by libcurl
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *s) {
    size_t newLength = size * nmemb;
    try {
        s->append((char*)contents, newLength);
        return newLength;
    }
    catch(std::bad_alloc &e) {
        // Handle memory allocation error
        return 0;
    }
}

// Function to make HTTP GET request using libcurl
std::string httpGet(const std::string &url) {
    CURL *curl;
    CURLcode res;
    std::string readBuffer;

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0"); // Set user agent
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if(res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }
    }
    return readBuffer;
}

// Function to extract component name and version using regex
std::pair<std::string, std::string> extractComponentInfo(const std::string &fileName) {
    std::regex re("org\\.mixedrealitytoolkit\\.(.+?)-([0-9]+\\.[0-9]+\\.[0-9]+(?:-pre\\.[0-9]+)?)\\.tgz");
    std::smatch match;
    if (std::regex_search(fileName, match, re) && match.size() > 2) {
        return {match[1].str(), match[2].str()};
    } 
    return {"", ""};
}

std::string findDownloadUrlForComponent(
    const std::string& component_name, 
    const std::string& version, 
    const nlohmann::json& jsonParsed) {

    // Regular expression to match the component filename
    std::regex componentRegex("org\\.mixedrealitytoolkit\\." + component_name + "-" + version + "\\.tgz");

    for (const auto& release : jsonParsed) {
        if (release.contains("assets")) {
            for (const auto& asset : release["assets"]) {
                std::string fileName = asset.value("name", "");
                if (std::regex_match(fileName, componentRegex)) {
                    return asset.value("browser_download_url", "");
                }
            }
        }
    }

    return ""; // Return empty string if no URL is found
}

void extractArchive(const std::string& archiveFile, const std::string& extractPath) {
    struct archive *a;
    struct archive_entry *entry;
    int r;

    a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    r = archive_read_open_filename(a, archiveFile.c_str(), 10240); // Block size

    if (r != ARCHIVE_OK) {
        std::cerr << "Error opening archive: " << archive_error_string(a) << std::endl;
        archive_read_free(a);
        return;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string fullPath = extractPath + "/" + archive_entry_pathname(entry);
        std::filesystem::path path(fullPath);
        std::filesystem::create_directories(path.parent_path()); // Create directories if they don't exist

        if (archive_entry_filetype(entry) == AE_IFREG) { // Regular file
            std::ofstream outputFile(fullPath, std::ios::binary);
            if (!outputFile) {
                std::cerr << "Error: Cannot open file " << fullPath << " for writing." << std::endl;
                continue;
            }

            const void *buff;
            size_t size;
            la_int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                outputFile.write(static_cast<const char*>(buff), size);
            }
        }
    }

    r = archive_read_free(a);  // Note: archive_read_close is not needed since archive_read_free calls it
    if (r != ARCHIVE_OK) {
        std::cerr << "Error closing archive: " << archive_error_string(a) << std::endl;
    }
}

void createTgzArchive(const std::string& folderPath, const std::string& tgzFileName) {
    struct archive *a;
    struct archive_entry *entry;
    struct stat st;
    char buff[8192];
    int len;
    int fd;

    // Extract the directory name from the folder path
    std::string dirName = std::filesystem::path(folderPath).filename();

    a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    archive_write_open_filename(a, tgzFileName.c_str());

    for (const auto & file : std::filesystem::recursive_directory_iterator(folderPath)) {
        const std::string filePath = file.path().string();
        // Include the directory name in the path inside the archive
        const std::string name = dirName + "/" + filePath.substr(folderPath.length() + 1);

        stat(filePath.c_str(), &st);
        entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_size(entry, st.st_size);
        archive_entry_set_filetype(entry, file.is_directory() ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry, file.is_directory() ? 0755 : 0644);
        archive_write_header(a, entry);

        if (file.is_regular_file()) {
            fd = open(filePath.c_str(), O_RDONLY);
            len = read(fd, buff, sizeof(buff));
            while ( len > 0 ) {
                archive_write_data(a, buff, len);
                len = read(fd, buff, sizeof(buff));
            }
            close(fd);
        }

        archive_entry_free(entry);
    }

    archive_write_close(a);
    archive_write_free(a);
}

void extractAndRepackage(const std::string& downloadedFile, const std::string& version) {
    // Step 1: Extract the downloaded .tar.gz file
    std::string extractPath = "./extracted"; // Temporary directory for extraction
    std::filesystem::create_directories(extractPath);
    // Function to extract files goes here (implement using libarchive)
    extractArchive(downloadedFile, extractPath);

    // Step 2: Locate the com.microsoft.mrtk.graphicstools.unity subfolder
    std::string subfolderPath = extractPath + "/MixedReality-GraphicsTools-Unity-" + version + "/com.microsoft.mrtk.graphicstools.unity";
    std::string packagePath = "./package";
    if (std::filesystem::exists(subfolderPath)) {
        std::filesystem::rename(subfolderPath, packagePath);
    }

    // Step 3: Archive the package folder as .tgz
    std::string tgzFileName = "com.microsoft.mrtk.graphicstools.unity-" + version + ".tgz";
    // Function to create .tgz archive goes here (implement using libarchive)
    createTgzArchive(packagePath, tgzFileName); 

    // Cleanup: Remove the extracted directory
    std::filesystem::remove_all(extractPath);

    std::filesystem::remove_all(packagePath);

    // Cleanup: Remove the downloaded .tgz file
    std::filesystem::remove(downloadedFile);
}

// Function to download and process dependencies
void downloadAndProcessDependencies(
    const std::string& component,
    const std::string& version,
    const nlohmann::json& jsonParsed,
    std::set<std::string>& processedComponents) {

    std::string componentKey = component + "-" + version;
    if (downloadedComponents.find(component) != downloadedComponents.end()) {
        if (!isNewerVersion(downloadedComponents[component], version)) {
            return; // Existing version is newer or the same, no need to download
        } else {
            // Delete the old .tgz file
            std::filesystem::remove("org.mixedrealitytoolkit." + component + "-" + downloadedComponents[component] + ".tgz");

        }
    }

    std::string downloadUrl = findDownloadUrlForComponent(component, version, jsonParsed);
    if (!downloadUrl.empty()) {
        std::string downloadedFile = downloadFile(downloadUrl);
        if (!downloadedFile.empty()) {
            downloadedComponents[component] = version; // Update the map with the new version
            auto dependencies = getDependencies(downloadUrl, component, version);
            processedComponents.insert(componentKey);

            // Process each dependency
            for (const auto& dependency : dependencies) {
                std::cout << "Dependency: " << dependency.first << ", Version: " << dependency.second << std::endl;
                if (dependency.first.starts_with("org.mixedrealitytoolkit")) {
                    std::string depComponent = dependency.first.substr(24); // Extract component name
                    downloadAndProcessDependencies(depComponent, dependency.second, jsonParsed, processedComponents);
                }
                else if (dependency.first.starts_with("com.microsoft.mrtk.graphicstools.unity"))
                {
                    if (std::filesystem::exists("com.microsoft.mrtk.graphicstools.unity-" + dependency.second + ".tgz")) {
                        std::cout << "Dependency already downloaded: " << dependency.first << std::endl;
                        continue;
                    }
                    std::string component = dependency.first;
                    std::string version = dependency.second;
                    std::string url = "https://github.com/microsoft/MixedReality-GraphicsTools-Unity/archive/refs/tags/v" + version +".tar.gz";
                    std::string downloadedDep = downloadFile(url);
                    if (!downloadedDep.empty()) {
                        downloadedComponents[component] = version; // Update the map with the new version
                        extractAndRepackage(downloadedDep, version);
                    }

                }
                
                
            }
        }
    } else {
        std::cerr << "Component not found: " << component << std::endl;
    }
}

int main() {
    std::string github_api_url = "https://api.github.com/repos/MixedRealityToolkit/MixedRealityToolkit-Unity/releases";
    std::string jsonResponse = httpGet(github_api_url);

    try {
        auto jsonParsed = nlohmann::json::parse(jsonResponse);
        std::map<std::string, std::vector<std::string>> mrtk_components;

        for (const auto &release : jsonParsed) {
            for (const auto &asset : release["assets"]) {
                std::string fileName = asset["name"];
                if (fileName.find(".tgz") != std::string::npos) {
                    auto [component, version] = extractComponentInfo(fileName);
                    if (!component.empty()) {
                        mrtk_components[component].push_back(version);
                    }
                }
            }
        }

        // Print components and versions
        int index = 0;
        for (const auto &pair : mrtk_components) {
            const auto &component = pair.first;
            componentNames.push_back(component); // Add component name to the vector

            std::cout << "[" << index++ << "] " << "Component: " << component << ", Versions: ";
            for (const auto &version : pair.second) {
                std::cout << version << " ";
            }
            std::cout << std::endl;
        }

        // User selection by index
        std::cout << "Enter the index numbers of the components to download (type -1 to finish): ";
        std::vector<int> selectedIndices;
        int inputIndex;
        while (std::cin >> inputIndex && inputIndex != -1) {
            if (inputIndex >= 0 && inputIndex < componentNames.size()) {
                selectedIndices.push_back(inputIndex);
            } else {
                std::cout << "Invalid index. Please try again: ";
            }
        }

        // Processing each selected component
        for (int idx : selectedIndices) {
            std::string component = componentNames[idx];
            // std::cout << "Enter version for " << component << " (or l for the latest version): ";
            std::string version = "l";
            // std::cin >> version;

            // Determine the latest version if needed
            if (version == "l") {
                auto it = mrtk_components.find(component);
                if (it != mrtk_components.end() && !it->second.empty()) {
                    std::sort(it->second.begin(), it->second.end(), [](const std::string &a, const std::string &b) {
                        return isNewerVersion(b, a); // Use your isNewerVersion function
                    });
                    version = it->second.front();
                }
                else {
                    std::cerr << "Component not found or has no versions: " << component << std::endl;
                    continue;
                }
            }

            // Check if the version is valid
            auto& versions = mrtk_components[component];
            if (std::find(versions.begin(), versions.end(), version) == versions.end() && version != "latest") {
                std::cerr << "Invalid version for component " << component << ": " << version << std::endl;
                continue;
            }

            // Download and process the component
            std::set<std::string> processedComponents;
            downloadAndProcessDependencies(component, version, jsonParsed, processedComponents);
        }

        // Find all tgz files and move them to a folder
        std::filesystem::create_directories("MixedReality");
        for (const auto &file : std::filesystem::directory_iterator(".")) {
            if (file.path().extension() == ".tgz") {
                std::filesystem::rename(file.path(), "MixedReality/" + file.path().filename().string());
            }
        }

    }
    catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}

