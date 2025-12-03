#include "json.h"
#include "xml.h"
#include "fomod.cpp"
#include "installer.cpp"
#include <iostream>

int main() {
    std::string sourceRoot = "test_fomod_env/extracted";
    std::string destRoot = "test_fomod_env/installed";
    
    // Need to find the root containing 'fomod'
    // Sometimes it's nested.
    // Simple crawler to find 'ModuleConfig.xml'
    std::string fomodRoot = sourceRoot;
    for (const auto& entry : fs::recursive_directory_iterator(sourceRoot)) {
        if (entry.path().filename() == "ModuleConfig.xml") {
            fomodRoot = entry.path().parent_path().parent_path().string();
            break;
        }
    }
    
    std::cout << "Detected FOMOD Root: " << fomodRoot << std::endl;

    // Dummy choices to ensure parsing works
    std::string jsonChoices = R"({"options": []})";
    auto rootOpt = TinyJson::Parser::parse(jsonChoices);

    if (FomodParser::process(fomodRoot, destRoot, *rootOpt)) {
        std::cout << "FOMOD Parse SUCCESS" << std::endl;
    } else {
        std::cout << "FOMOD Parse FAILED" << std::endl;
    }

    return 0;
}
