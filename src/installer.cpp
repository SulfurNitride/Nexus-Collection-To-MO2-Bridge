#pragma once
#include <iostream>
#include <cstdlib>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

class Installer {
public:
    static std::string get7zCommand() {
        #ifdef _WIN32
            // Prefer full 7z.exe (has RAR support via 7z.dll)
            if (fs::exists("7z.exe")) {
                return "7z.exe";
            }
            // Fallback to 7za.exe (standalone, no RAR)
            if (fs::exists("7za.exe")) {
                return "7za.exe";
            }
        #else
            const std::string bundled = "./7zzs";
            if (fs::exists(bundled)) {
                // Ensure it's executable on Linux
                std::string chmod = "chmod +x " + bundled;
                (void)std::system(chmod.c_str());
                return bundled;
            }
        #endif
        return "7z"; // Fallback to global
    }

    static bool extract(const std::string& archivePath, const std::string& destPath) {
        // Using 7zip as an external command
        // "7z x <archive> -o<dest> -y"
        if (!fs::exists(archivePath)) return false;
        if (!fs::exists(destPath)) fs::create_directories(destPath);

        std::string cmd = get7zCommand() + " x \"" + archivePath + "\" -o\"" + destPath + "\" -y > /dev/null";
        int ret = std::system(cmd.c_str());
        return ret == 0;
    }

    static bool install(const std::string& archivePath, const std::string& modsDir, const std::string& modName) {
        std::string dest = modsDir + "/" + modName;
        std::cout << "Installing " << modName << " to " << dest << std::endl;
        return extract(archivePath, dest);
    }
};

