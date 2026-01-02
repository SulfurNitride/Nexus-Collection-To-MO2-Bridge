#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include "../include/nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace FomodInstaller {

// Represents a single choice within a group
struct Choice {
    std::string name;
    int idx;
};

// Represents a group of choices within a step
struct Group {
    std::string name;
    std::vector<Choice> choices;
};

// Represents a step in the FOMOD installer
struct Step {
    std::string name;
    std::vector<Group> groups;
};

// Parsed FOMOD choices from collection.json
struct FomodChoices {
    std::vector<Step> steps;

    // Check if a specific option is selected
    // Uses step name + group name as composite key to avoid collisions
    bool isSelected(const std::string& stepName, const std::string& groupName,
                    const std::string& optionName) const;

    // Get all selected option names for a step/group
    std::set<std::string> getSelectedOptions(const std::string& stepName,
                                              const std::string& groupName) const;
};

// Parse FOMOD choices from collection.json mod entry
FomodChoices parseChoices(const json& choicesJson);

// Process a FOMOD installer
// sourceRoot: extracted mod directory containing fomod/ModuleConfig.xml
// destRoot: destination directory for installed files
// choices: parsed choices from collection.json
// Returns true on success
bool process(const std::string& sourceRoot, const std::string& destRoot,
             const FomodChoices& choices);

// Find ModuleConfig.xml in a mod directory (case-insensitive)
fs::path findModuleConfig(const fs::path& modRoot);

} // namespace FomodInstaller
