#pragma once
#include "json.h"
#include "xml.h"
#include <string>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>

namespace fs = std::filesystem;

class FomodParser {
public:
    static bool process(const std::string& sourceRoot, const std::string& destRoot, const TinyJson::Value& choices) {
        fs::path xmlPath;
        bool found = false;
        std::string effectiveSourceRoot = sourceRoot;

        // Helper to find file case-insensitive
        auto findFile = [&](const fs::path& dir, const std::string& target) -> fs::path {
            if (!fs::exists(dir)) return "";
            for (const auto& entry : fs::directory_iterator(dir)) {
                std::string name = entry.path().filename().string();
                std::string lowerName = name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                std::string targetLower = target;
                std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(), ::tolower);
                if (lowerName == targetLower) return entry.path();
            }
            return "";
        };

        // Helper to find FOMOD xml in a directory (checks root and fomod/ subfolder)
        auto findFomodXml = [&](const fs::path& dir) -> fs::path {
            // Check root
            fs::path xml = findFile(dir, "ModuleConfig.xml");
            if (!xml.empty()) return xml;
            // Check fomod/
            fs::path fomodDir = findFile(dir, "fomod");
            if (!fomodDir.empty()) {
                xml = findFile(fomodDir, "ModuleConfig.xml");
                if (!xml.empty()) return xml;
            }
            return "";
        };

        // First, try to find FOMOD in sourceRoot directly
        xmlPath = findFomodXml(sourceRoot);
        if (!xmlPath.empty()) {
            found = true;
        }

        // If not found, check if archive has a single wrapper folder
        // (common pattern: archive extracts to "ModName/fomod/ModuleConfig.xml")
        if (!found) {
            std::vector<fs::path> subdirs;
            int fileCount = 0;
            for (const auto& entry : fs::directory_iterator(sourceRoot)) {
                if (entry.is_directory()) {
                    subdirs.push_back(entry.path());
                } else {
                    fileCount++;
                }
            }
            // If there's exactly one subdirectory and few/no files, check inside it
            if (subdirs.size() == 1 && fileCount <= 2) {
                fs::path wrapperDir = subdirs[0];
                xmlPath = findFomodXml(wrapperDir);
                if (!xmlPath.empty()) {
                    effectiveSourceRoot = wrapperDir.string();
                    found = true;
                    std::cout << "  Detected wrapper folder: " << wrapperDir.filename() << std::endl;
                }
            }
        }

        if (!found) return false;

        std::cout << "Processing FOMOD from: " << effectiveSourceRoot << " to " << destRoot << std::endl;

        // 1. Build Map of User Choices from JSON
        // Structure: choices -> options[] -> groups[] -> choices[] -> name
        // Use multimap to support multiple selections per group
        std::multimap<std::string, std::string> userSelections;
        if (choices.isObject() && choices["options"].isArray()) {
            for (const auto& step : choices["options"].asArray()) {
                if (step["groups"].isArray()) {
                    for (const auto& group : step["groups"].asArray()) {
                        std::string groupName = group["name"].asString();
                        if (group["choices"].isArray()) {
                            for (const auto& choice : group["choices"].asArray()) {
                                std::string optionName = choice["name"].asString();
                                if (!groupName.empty() && !optionName.empty()) {
                                    userSelections.insert({groupName, optionName});
                                }
                            }
                        }
                    }
                }
            }
        }

        // Read XML with encoding handling
        std::ifstream t(xmlPath, std::ios::binary);
        std::vector<char> buffer((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
        std::string xmlContent;

        if (buffer.size() >= 2 && (unsigned char)buffer[0] == 0xFF && (unsigned char)buffer[1] == 0xFE) {
            // UTF-16 LE: Crude conversion by taking every other byte
            for (size_t i = 2; i < buffer.size(); i += 2) {
                xmlContent += buffer[i];
            }
        } else if (buffer.size() >= 3 && (unsigned char)buffer[0] == 0xEF && (unsigned char)buffer[1] == 0xBB && (unsigned char)buffer[2] == 0xBF) {
            // UTF-8 BOM: Skip first 3 bytes
            xmlContent.assign(buffer.begin() + 3, buffer.end());
        } else {
            // Assume UTF-8 or ASCII
            xmlContent.assign(buffer.begin(), buffer.end());
        }

        auto root = TinyXML::Parser::parse(xmlContent);

        if (!root) {
            std::cerr << "Failed to parse ModuleConfig.xml" << std::endl;
            return false;
        }

        // Helper for case-insensitive comparison
        auto iequals = [](const std::string& a, const std::string& b) -> bool {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        };

        // 2. Execute Install Steps
        auto steps = root->findChild("installSteps");
        if (steps) {
            // FIXED: Use "installStep" instead of "step"
            for (const auto& step : steps->findChildren("installStep")) {
                auto optFileGroups = step->findChild("optionalFileGroups");
                if (optFileGroups) {
                    for (const auto& group : optFileGroups->findChildren("group")) {
                        std::string groupName = group->getAttribute("name");

                        // Get all selected options for this group (multimap allows multiple)
                        auto range = userSelections.equal_range(groupName);

                        auto plugins = group->findChild("plugins");
                        if (plugins) {
                            for (const auto& plugin : plugins->findChildren("plugin")) {
                                std::string pluginName = plugin->getAttribute("name");

                                // Check if this plugin is in any of the selected options (case-insensitive)
                                bool isSelected = false;
                                for (auto it = range.first; it != range.second; ++it) {
                                    if (iequals(pluginName, it->second)) {
                                        isSelected = true;
                                        break;
                                    }
                                }

                                if (isSelected) {
                                    std::cout << "  [+] Installing Option: " << pluginName << std::endl;
                                    installPluginFiles(plugin, effectiveSourceRoot, destRoot);
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Required Files?
        auto required = root->findChild("requiredInstallFiles");
        if (required) {
            for (const auto& file : required->findChildren("file")) {
                 installFile(file, effectiveSourceRoot, destRoot);
            }
             for (const auto& folder : required->findChildren("folder")) {
                 installFolder(folder, effectiveSourceRoot, destRoot);
            }
        }
        
        return true;
    }

private:
    static void installPluginFiles(const std::shared_ptr<TinyXML::Element>& plugin, const std::string& srcRoot, const std::string& dstRoot) {
        auto files = plugin->findChild("files");
        if (!files) {
             for (const auto& file : plugin->findChildren("file")) installFile(file, srcRoot, dstRoot);
             for (const auto& folder : plugin->findChildren("folder")) installFolder(folder, srcRoot, dstRoot);
             return;
        }
        for (const auto& file : files->findChildren("file")) installFile(file, srcRoot, dstRoot);
        for (const auto& folder : files->findChildren("folder")) installFolder(folder, srcRoot, dstRoot);
    }

    // Helper to find file case-insensitively (single component)
    static fs::path findCaseInsensitive(const fs::path& dir, const std::string& target) {
        if (!fs::exists(dir)) return "";
        std::string targetLower = target;
        std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(), ::tolower);
        for (const auto& entry : fs::directory_iterator(dir)) {
            std::string name = entry.path().filename().string();
            std::string nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            if (nameLower == targetLower) return entry.path();
        }
        return "";
    }

    // Normalize path separators
    static std::string normalizePath(const std::string& path) {
        std::string result = path;
        std::replace(result.begin(), result.end(), '\\', '/');
        return result;
    }

    // Recursively resolve a relative path case-insensitively from a base directory
    static fs::path resolveCaseInsensitive(const fs::path& base, const std::string& relativePath) {
        std::string normalized = normalizePath(relativePath);

        // Split path into components
        std::vector<std::string> components;
        std::stringstream ss(normalized);
        std::string component;
        while (std::getline(ss, component, '/')) {
            if (!component.empty()) {
                components.push_back(component);
            }
        }

        if (components.empty()) return base;

        fs::path current = base;
        for (const auto& comp : components) {
            if (!fs::exists(current)) return "";

            // Try exact match first
            fs::path exact = current / comp;
            if (fs::exists(exact)) {
                current = exact;
                continue;
            }

            // Try case-insensitive match
            fs::path found = findCaseInsensitive(current, comp);
            if (found.empty()) return "";
            current = found;
        }

        return current;
    }

    static void installFile(const std::shared_ptr<TinyXML::Element>& fileEl, const std::string& srcRoot, const std::string& dstRoot) {
        std::string src = normalizePath(fileEl->getAttribute("source"));
        std::string dst = fileEl->getAttribute("destination");
        if (dst.empty()) dst = src;
        dst = normalizePath(dst);
        if (src.empty()) return;

        // Use recursive case-insensitive path resolution
        fs::path sourcePath = resolveCaseInsensitive(srcRoot, src);
        fs::path destPath = fs::path(dstRoot) / dst;

        try {
            if (!sourcePath.empty() && fs::exists(sourcePath) && !fs::is_directory(sourcePath)) {
                fs::create_directories(destPath.parent_path());
                fs::copy_file(sourcePath, destPath, fs::copy_options::overwrite_existing);
            }
        } catch (...) {}
    }

    static void installFolder(const std::shared_ptr<TinyXML::Element>& folderEl, const std::string& srcRoot, const std::string& dstRoot) {
        std::string src = normalizePath(folderEl->getAttribute("source"));
        std::string dst = folderEl->getAttribute("destination");
        if (dst.empty()) dst = src;
        dst = normalizePath(dst);
        if (src.empty()) return;

        // Use recursive case-insensitive path resolution
        fs::path sourcePath = resolveCaseInsensitive(srcRoot, src);
        fs::path destPath = fs::path(dstRoot) / dst;

        try {
            if (!sourcePath.empty() && fs::exists(sourcePath) && fs::is_directory(sourcePath)) {
                fs::create_directories(destPath);
                fs::copy(sourcePath, destPath, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }
        } catch (...) {}
    }
};
