#include "fomod_installer.hpp"
#include "../include/pugixml/pugixml.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace FomodInstaller {

// Case-insensitive string comparison
static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Normalize path separators and case for comparison
static std::string normalizePath(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

// Resolve a path case-insensitively by walking each segment
// Returns empty path if resolution fails
static fs::path resolveCaseInsensitive(const fs::path& base, const std::string& relativePath) {
    std::string normalized = relativePath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    fs::path currentPath = base;
    std::istringstream pathStream(normalized);
    std::string segment;

    while (std::getline(pathStream, segment, '/')) {
        if (segment.empty()) continue;

        // First try exact match
        fs::path directPath = currentPath / segment;
        if (fs::exists(directPath)) {
            currentPath = directPath;
            continue;
        }

        // Try case-insensitive match
        std::string segmentLower = segment;
        std::transform(segmentLower.begin(), segmentLower.end(), segmentLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        bool found = false;
        if (fs::exists(currentPath) && fs::is_directory(currentPath)) {
            for (const auto& entry : fs::directory_iterator(currentPath)) {
                std::string entryName = entry.path().filename().string();
                std::string entryLower = entryName;
                std::transform(entryLower.begin(), entryLower.end(), entryLower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (entryLower == segmentLower) {
                    currentPath = entry.path();
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            return fs::path(); // Resolution failed
        }
    }

    return currentPath;
}

bool FomodChoices::isSelected(const std::string& stepName, const std::string& groupName,
                               const std::string& optionName) const {
    for (const auto& step : steps) {
        if (iequals(step.name, stepName)) {
            for (const auto& group : step.groups) {
                if (iequals(group.name, groupName)) {
                    for (const auto& choice : group.choices) {
                        if (iequals(choice.name, optionName)) {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

std::set<std::string> FomodChoices::getSelectedOptions(const std::string& stepName,
                                                        const std::string& groupName) const {
    std::set<std::string> result;
    for (const auto& step : steps) {
        if (iequals(step.name, stepName)) {
            for (const auto& group : step.groups) {
                if (iequals(group.name, groupName)) {
                    for (const auto& choice : group.choices) {
                        result.insert(choice.name);
                    }
                }
            }
        }
    }
    return result;
}

FomodChoices parseChoices(const json& choicesJson) {
    FomodChoices result;

    if (!choicesJson.contains("options") || !choicesJson["options"].is_array()) {
        return result;
    }

    for (const auto& stepJson : choicesJson["options"]) {
        Step step;
        step.name = stepJson.value("name", "");

        if (stepJson.contains("groups") && stepJson["groups"].is_array()) {
            for (const auto& groupJson : stepJson["groups"]) {
                Group group;
                group.name = groupJson.value("name", "");

                if (groupJson.contains("choices") && groupJson["choices"].is_array()) {
                    for (const auto& choiceJson : groupJson["choices"]) {
                        Choice choice;
                        choice.name = choiceJson.value("name", "");
                        choice.idx = choiceJson.value("idx", 0);
                        // Allow empty choice names - some FOMODs use empty-name plugins
                        group.choices.push_back(choice);
                    }
                }

                // Always add the group, even with empty name
                step.groups.push_back(group);
            }
        }

        // Always add the step, even with empty name (some FOMODs use empty step names)
        result.steps.push_back(step);
    }

    return result;
}

fs::path findModuleConfig(const fs::path& modRoot) {
    // Search recursively for fomod/ModuleConfig.xml
    // This handles archives with nested folder structures
    try {
        for (const auto& entry : fs::recursive_directory_iterator(modRoot)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string lower = filename;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                if (lower == "moduleconfig.xml") {
                    // Verify it's in a fomod folder
                    std::string parentName = entry.path().parent_path().filename().string();
                    std::string parentLower = parentName;
                    std::transform(parentLower.begin(), parentLower.end(), parentLower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (parentLower == "fomod") {
                        return entry.path();
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "  [WARN] Error searching for ModuleConfig.xml: " << e.what() << std::endl;
    }

    return fs::path();
}

// Install a single file
static void installFile(const pugi::xml_node& fileNode, const fs::path& srcRoot,
                        const fs::path& dstRoot) {
    std::string src = normalizePath(fileNode.attribute("source").as_string());
    std::string dst = fileNode.attribute("destination").as_string();
    // When destination is empty, use just the filename (not the full source path)
    if (dst.empty()) {
        dst = fs::path(src).filename().string();
    }
    dst = normalizePath(dst);

    if (src.empty()) return;

    // Handle root destination markers (Windows \ or /) - use just the filename
    if (dst == "/" || dst == "\\") {
        dst = fs::path(src).filename().string();
    }

    fs::path sourcePath = srcRoot / src;
    fs::path destPath = dstRoot / dst;

    try {
        // Handle case-insensitive file matching for nested paths like "95 Merged ESP SE All/SMIM-SE-Merged-All.esp"
        if (!fs::exists(sourcePath)) {
            fs::path resolved = resolveCaseInsensitive(srcRoot, src);
            if (!resolved.empty() && fs::exists(resolved)) {
                sourcePath = resolved;
            }
        }

        if (fs::exists(sourcePath) && !fs::is_directory(sourcePath)) {
            fs::create_directories(destPath.parent_path());
            fs::copy_file(sourcePath, destPath, fs::copy_options::overwrite_existing);
        }
    } catch (const std::exception& e) {
        std::cerr << "  [WARN] Failed to copy file: " << src << " -> " << dst
                  << " (" << e.what() << ")" << std::endl;
    }
}

// Find existing folder with case-insensitive match in destination
static fs::path findExistingFolder(const fs::path& destDir, const std::string& folderName) {
    if (!fs::exists(destDir) || !fs::is_directory(destDir)) {
        return fs::path();
    }

    std::string nameLower = folderName;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& entry : fs::directory_iterator(destDir)) {
        if (entry.is_directory()) {
            std::string entryName = entry.path().filename().string();
            std::string entryLower = entryName;
            std::transform(entryLower.begin(), entryLower.end(), entryLower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (entryLower == nameLower) {
                return entry.path();
            }
        }
    }
    return fs::path();
}

// Recursively copy directory with case-insensitive merging
static void copyDirMerge(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(dst)) {
        fs::create_directories(dst);
    }

    for (const auto& entry : fs::directory_iterator(src)) {
        std::string itemName = entry.path().filename().string();

        if (entry.is_directory()) {
            // Check for case-insensitive match in destination
            fs::path existingDir = findExistingFolder(dst, itemName);
            if (!existingDir.empty()) {
                // Merge into existing folder
                copyDirMerge(entry.path(), existingDir);
            } else {
                // Create new folder and copy
                fs::path newDir = dst / itemName;
                copyDirMerge(entry.path(), newDir);
            }
        } else {
            // Copy file, overwriting if exists
            fs::path target = dst / itemName;
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
}

// Install a folder recursively
static void installFolder(const pugi::xml_node& folderNode, const fs::path& srcRoot,
                          const fs::path& dstRoot) {
    std::string src = normalizePath(folderNode.attribute("source").as_string());
    std::string dst = folderNode.attribute("destination").as_string();
    dst = normalizePath(dst);

    if (src.empty()) return;

    // Handle root destination markers (Windows \ or /) as empty (mod root)
    if (dst == "/" || dst == "\\") {
        dst = "";
    }

    fs::path sourcePath = srcRoot / src;
    fs::path destPath = dstRoot / dst;

    // Debug: show what we're trying to copy
    std::cout << "        [folder] src=\"" << src << "\" -> dst=\"" << (dst.empty() ? "(root)" : dst) << "\"" << std::endl;

    try {
        if (!fs::exists(sourcePath)) {
            // Try case-insensitive path resolution for nested paths like "00 Core/Meshes"
            fs::path resolved = resolveCaseInsensitive(srcRoot, src);
            if (!resolved.empty() && fs::exists(resolved)) {
                sourcePath = resolved;
            }
        }

        if (fs::exists(sourcePath) && fs::is_directory(sourcePath)) {
            // When destination is empty or root, copy contents of source folder to dest root
            // When destination is specified, copy contents to that destination path
            if (!destPath.empty()) {
                fs::create_directories(destPath);
            }

            int copied = 0;
            for (const auto& entry : fs::directory_iterator(sourcePath)) {
                std::string itemName = entry.path().filename().string();

                if (fs::is_directory(entry)) {
                    // Check for case-insensitive match in destination
                    fs::path existingDir = findExistingFolder(destPath, itemName);
                    if (!existingDir.empty()) {
                        // Merge into existing folder
                        copyDirMerge(entry.path(), existingDir);
                    } else {
                        // Create new folder and copy
                        fs::path target = destPath / itemName;
                        copyDirMerge(entry.path(), target);
                    }
                } else {
                    fs::path target = destPath / itemName;
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
                }
                copied++;
            }
            std::cout << "        [folder] Copied " << copied << " items from " << sourcePath.filename() << std::endl;
        } else {
            std::cerr << "        [WARN] Source folder not found: " << sourcePath << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "  [WARN] Failed to copy folder: " << src << " -> " << dst
                  << " (" << e.what() << ")" << std::endl;
    }
}

// Collect flags from a selected plugin's conditionFlags element
static void collectPluginFlags(const pugi::xml_node& plugin,
                                std::map<std::string, std::string>& flags) {
    pugi::xml_node conditionFlags = plugin.child("conditionFlags");
    if (conditionFlags) {
        for (pugi::xml_node flag : conditionFlags.children("flag")) {
            std::string flagName = flag.attribute("name").as_string();
            std::string flagValue = flag.child_value();
            if (!flagName.empty()) {
                flags[flagName] = flagValue;
            }
        }
    }
}

// Check if a single flag dependency is satisfied
static bool checkFlagDependency(const pugi::xml_node& flagDep,
                                 const std::map<std::string, std::string>& flags) {
    std::string flagName = flagDep.attribute("flag").as_string();
    std::string requiredValue = flagDep.attribute("value").as_string();

    auto it = flags.find(flagName);
    if (it == flags.end()) {
        return false;  // Flag not set
    }
    return iequals(it->second, requiredValue);
}

// Evaluate dependencies for a conditional pattern
static bool evaluateDependencies(const pugi::xml_node& dependencies,
                                  const std::map<std::string, std::string>& flags) {
    std::string op = dependencies.attribute("operator").as_string();
    bool isAnd = (op.empty() || iequals(op, "And"));  // Default is And

    bool hasAny = false;
    for (pugi::xml_node flagDep : dependencies.children("flagDependency")) {
        hasAny = true;
        bool satisfied = checkFlagDependency(flagDep, flags);

        if (isAnd && !satisfied) {
            return false;  // And: any false means overall false
        }
        if (!isAnd && satisfied) {
            return true;   // Or: any true means overall true
        }
    }

    // Handle nested dependencies
    for (pugi::xml_node nestedDeps : dependencies.children("dependencies")) {
        hasAny = true;
        bool satisfied = evaluateDependencies(nestedDeps, flags);

        if (isAnd && !satisfied) {
            return false;
        }
        if (!isAnd && satisfied) {
            return true;
        }
    }

    // And: all true (or no deps) = true; Or: all false = false
    return isAnd || !hasAny;
}

// Install files from a conditional pattern
static void installPatternFiles(const pugi::xml_node& pattern, const fs::path& srcRoot,
                                 const fs::path& dstRoot) {
    pugi::xml_node files = pattern.child("files");
    if (!files) return;

    for (pugi::xml_node file : files.children("file")) {
        installFile(file, srcRoot, dstRoot);
    }
    for (pugi::xml_node folder : files.children("folder")) {
        installFolder(folder, srcRoot, dstRoot);
    }
}

// Install files from a plugin node
static void installPluginFiles(const pugi::xml_node& plugin, const fs::path& srcRoot,
                                const fs::path& dstRoot) {
    // Check for <files> container
    pugi::xml_node filesNode = plugin.child("files");
    if (filesNode) {
        for (pugi::xml_node file : filesNode.children("file")) {
            installFile(file, srcRoot, dstRoot);
        }
        for (pugi::xml_node folder : filesNode.children("folder")) {
            installFolder(folder, srcRoot, dstRoot);
        }
    } else {
        // Files might be direct children
        for (pugi::xml_node file : plugin.children("file")) {
            installFile(file, srcRoot, dstRoot);
        }
        for (pugi::xml_node folder : plugin.children("folder")) {
            installFolder(folder, srcRoot, dstRoot);
        }
    }
}

bool process(const std::string& sourceRoot, const std::string& destRoot,
             const FomodChoices& choices) {

    fs::path xmlPath = findModuleConfig(sourceRoot);
    if (xmlPath.empty()) {
        std::cerr << "  [ERROR] ModuleConfig.xml not found in: " << sourceRoot << std::endl;
        return false;
    }

    std::cout << "  Processing FOMOD: " << xmlPath << std::endl;

    // Source root is the parent of the fomod folder (where FOMOD data files are)
    // xmlPath = .../fomod/ModuleConfig.xml, so parent.parent = data root
    fs::path srcRoot = xmlPath.parent_path().parent_path();
    std::cout << "    Source root: " << srcRoot << std::endl;

    // Load XML with pugixml - handle various encodings including UTF-16
    pugi::xml_document doc;

    // Read file as binary first to detect encoding
    std::ifstream file(xmlPath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "  [ERROR] Failed to open XML file: " << xmlPath << std::endl;
        return false;
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    file.read(buffer.data(), size);
    file.close();

    // Detect encoding from BOM
    pugi::xml_encoding encoding = pugi::encoding_auto;
    if (size >= 2) {
        unsigned char b0 = static_cast<unsigned char>(buffer[0]);
        unsigned char b1 = static_cast<unsigned char>(buffer[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
            encoding = pugi::encoding_utf16_le;
            std::cout << "    Detected UTF-16 LE encoding" << std::endl;
        } else if (b0 == 0xFE && b1 == 0xFF) {
            encoding = pugi::encoding_utf16_be;
            std::cout << "    Detected UTF-16 BE encoding" << std::endl;
        }
    }

    // Parse with detected encoding
    pugi::xml_parse_result result = doc.load_buffer(buffer.data(), buffer.size(),
        pugi::parse_default | pugi::parse_declaration, encoding);

    if (!result) {
        std::cerr << "  [ERROR] Failed to parse XML: " << result.description() << std::endl;
        return false;
    }

    fs::path dstRoot = fs::path(destRoot);
    fs::create_directories(dstRoot);

    // Track flags set by selected plugins for conditional installs
    std::map<std::string, std::string> flags;

    // Get root config element
    pugi::xml_node config = doc.child("config");
    if (!config) {
        // Try without namespace
        config = doc.first_child();
    }

    if (!config) {
        std::cerr << "  [ERROR] Could not find config element in XML" << std::endl;
        return false;
    }
    std::cout << "    Config element: " << config.name() << std::endl;

    // Process requiredInstallFiles first
    pugi::xml_node requiredFiles = config.child("requiredInstallFiles");
    if (requiredFiles) {
        std::cout << "  Installing required files..." << std::endl;
        for (pugi::xml_node file : requiredFiles.children("file")) {
            installFile(file, srcRoot, dstRoot);
        }
        for (pugi::xml_node folder : requiredFiles.children("folder")) {
            installFolder(folder, srcRoot, dstRoot);
        }
    }

    // Process install steps
    pugi::xml_node installSteps = config.child("installSteps");
    if (installSteps) {
        for (pugi::xml_node step : installSteps.children("installStep")) {
            std::string stepName = step.attribute("name").as_string();
            if (stepName.empty()) {
                // Try alternate attribute name
                stepName = step.attribute("Name").as_string();
            }

            std::cout << "  Step: " << stepName << std::endl;

            pugi::xml_node optionalFileGroups = step.child("optionalFileGroups");
            if (!optionalFileGroups) continue;

            for (pugi::xml_node group : optionalFileGroups.children("group")) {
                std::string groupName = group.attribute("name").as_string();
                std::cout << "    Group: " << groupName << std::flush;

                // Get selected options for this step+group combination
                std::set<std::string> selectedOptions = choices.getSelectedOptions(stepName, groupName);
                std::cout << " (" << selectedOptions.size() << " selected)" << std::endl;

                pugi::xml_node plugins = group.child("plugins");
                if (!plugins) continue;

                int pluginIndex = 0;
                int pluginCount = 0;
                for (auto _ : plugins.children("plugin")) { (void)_; pluginCount++; }

                for (pugi::xml_node plugin : plugins.children("plugin")) {
                    std::string pluginName = plugin.attribute("name").as_string();

                    // Check if this plugin/option was selected
                    // Handle empty plugin names by checking if empty string is in selections
                    bool isSelected = false;
                    for (const auto& selected : selectedOptions) {
                        if (iequals(selected, pluginName)) {
                            isSelected = true;
                            break;
                        }
                    }

                    if (isSelected) {
                        std::cout << "      [+] Installing: " << (pluginName.empty() ? "(default)" : pluginName) << std::flush;
                        // Collect flags from selected plugin for conditional installs
                        collectPluginFlags(plugin, flags);
                        installPluginFiles(plugin, srcRoot, dstRoot);
                        std::cout << " - done" << std::endl;
                    }
                    pluginIndex++;
                }
            }
        }
    }

    // Process conditionalFileInstalls (for flag-based installs)
    pugi::xml_node conditionalInstalls = config.child("conditionalFileInstalls");
    if (conditionalInstalls) {
        std::cout << "  Processing conditional installs..." << std::endl;

        // Debug: print collected flags
        if (!flags.empty()) {
            std::cout << "    Flags: ";
            for (const auto& [name, value] : flags) {
                std::cout << name << "=" << value << " ";
            }
            std::cout << std::endl;
        }

        pugi::xml_node patterns = conditionalInstalls.child("patterns");
        if (patterns) {
            for (pugi::xml_node pattern : patterns.children("pattern")) {
                pugi::xml_node dependencies = pattern.child("dependencies");
                if (dependencies) {
                    if (evaluateDependencies(dependencies, flags)) {
                        std::cout << "      [+] Pattern matched, installing files..." << std::endl;
                        installPatternFiles(pattern, srcRoot, dstRoot);
                    }
                } else {
                    // No dependencies = always install
                    installPatternFiles(pattern, srcRoot, dstRoot);
                }
            }
        }
    }

    return true;
}

} // namespace FomodInstaller
