#include "fomod.cpp"
#include "xml.cpp"
#include "json.cpp"
#include <iostream>
#include <fstream>

int main() {
    // Test Case: More Consistent Map Markers (Based on JSON provided by user)
    // Since we don't have the exact files, we will point to the folder where it MIGHT be if extracted,
    // OR we just rely on the user confirming they have "More Consistent Map Markers" archive.
    // But wait, the user said "we have fomods at our disposal". 
    // I will try to find one that actually exists. 
    // The find command found: /home/luke/Documents/MO2/mods/Patches for Arthmoor's Town add-ons/...
    
    // Let's try to use THAT one if we can get the JSON choices for it. 
    // But I don't have the JSON choices for that specific mod handy in the context.
    // The user provided JSON for "More Consistent Map Markers".
    // Let's assume "More Consistent Map Markers" failed to install correctly previously or is available.
    
    // I will try to run the parser against the "Patches for Arthmoor's Town add-ons" path 
    // but with a DUMMY choice set just to see if it PARSES the XML without crashing.
    // Verify XML parsing capabilities.

    std::string modPath = "/home/luke/Documents/MO2/mods/Patches for Arthmoor's Town add-ons/Landscape For Grass Mods patches for Arthmoor's towns";
    // Adjust path to root of the mod where fomod folder is
    // It seems nested? 
    // Let's look at the path: .../mods/ModName/Landscape.../fomod/ModuleConfig.xml
    // This implies the mod content is inside a subfolder? Or is that just how it was installed?
    
    // Let's target the parent of 'fomod'
    std::string sourceRoot = "/home/luke/Documents/MO2/mods/Patches for Arthmoor's Town add-ons/Landscape For Grass Mods patches for Arthmoor's towns";
    std::string destRoot = "test_output_install";
    
    fs::create_directories(destRoot);

    // Dummy Choices (Empty) - just to test XML parsing and recursion
    std::string jsonChoices = R"({"options": []})";
    auto rootOpt = TinyJson::Parser::parse(jsonChoices);

    std::cout << "Testing FOMOD Parser on: " << sourceRoot << std::endl;
    
    if (FomodParser::process(sourceRoot, destRoot, *rootOpt)) {
        std::cout << "SUCCESS: XML Parsed and Processed (No options selected)." << std::endl;
    } else {
        std::cout << "FAILURE: XML Parsing or Processing failed." << std::endl;
    }
    
    fs::remove_all(destRoot);
    return 0;
}
