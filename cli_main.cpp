#include "core_logic.h"
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

void display_menu(const std::vector<SelectablePackage>& allPackages) {
    int currentIndex = 0;
    std::cout << "\nAvailable MRTK Components:\n------------------------\n";
    for (const auto& pkg : allPackages) {
        if (pkg.type == PackageType::MRTK) {
            std::cout << "[" << currentIndex++ << "] " << pkg.displayName << std::endl;
        }
    }
    std::cout << "\nOpenXR Runtimes:\n------------------------\n";
    for (const auto& pkg : allPackages) {
        if (pkg.type == PackageType::OpenXR) {
             std::cout << "[" << currentIndex++ << "] " << pkg.displayName << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: Please drag your Unity project folder onto the executable." << std::endl;
        std::cout << "Press enter to exit.";
        std::cin.get();
        return 1;
    }

    fs::path projectPath = argv[1];
    if (!fs::is_directory(projectPath) || !MRTKToolCore::isValidUnityProject(projectPath)) {
        std::cout << "The provided path is not a valid Unity project folder." << std::endl;
        std::cout << "Press enter to exit.";
        std::cin.get();
        return 1;
    }

    std::string currentUnityVersionStr = MRTKToolCore::getUnityVersion(projectPath);
    if (currentUnityVersionStr.empty()) {
        std::cerr << "Warning: Could not determine Unity editor version from ProjectSettings/ProjectVersion.txt" << std::endl;
    } else {
        std::cout << "Detected Unity Version: " << currentUnityVersionStr << std::endl;
    }

    try {
        MRTKToolCore tool;

        if (!tool.fetchAvailablePackages()) {
            std::cerr << "Could not retrieve package list. Exiting." << std::endl;
            return 1;
        }

        const auto& allPackages = tool.getAvailablePackages();
        display_menu(allPackages);

        std::cout << "\nEnter the index numbers of packages to install (e.g., 2 7 14): ";
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

        tool.resolveDependencies(selectedIndices);
        tool.downloadAndRepackage();
        tool.installPackagesToProject(projectPath);

    } catch (const std::exception& e) {
        std::cerr << "An unexpected error occurred: " << e.what() << std::endl;
        std::cout << "Press enter to exit.";
        std::cin.get();
        return 1;
    }
    
    std::cout << "\nOperation completed." << std::endl;
    return 0;
}