/**
 * Test for the new FOMOD installer implementation
 */

#include "fomod_installer.hpp"
#include <iostream>
#include <fstream>

namespace fs = std::filesystem;

int main() {
    std::cout << "=== FOMOD Installer Test ===" << std::endl;

    // Test case: Multiple steps with same group names
    // This tests the fix for the composite key issue
    std::string testJson = R"({
        "options": [
            {
                "name": "Installation Notice",
                "groups": [
                    {
                        "name": "Read first",
                        "choices": [
                            { "name": "Proceed", "idx": 0 }
                        ]
                    }
                ]
            },
            {
                "name": "Choose Marker version",
                "groups": [
                    {
                        "name": "Read first",
                        "choices": [
                            { "name": "Simplified", "idx": 0 }
                        ]
                    },
                    {
                        "name": "Color Variation",
                        "choices": [
                            { "name": "Non colored Main Cities", "idx": 1 }
                        ]
                    }
                ]
            },
            {
                "name": "Choose Simplified non-colored Compass",
                "groups": [
                    {
                        "name": "Choose your installed mod",
                        "choices": [
                            { "name": "Compass Navigation Overhaul", "idx": 2 }
                        ]
                    }
                ]
            }
        ],
        "type": "fomod"
    })";

    // Parse the choices
    json choicesJson = json::parse(testJson);
    FomodInstaller::FomodChoices choices = FomodInstaller::parseChoices(choicesJson);

    std::cout << std::endl << "Parsed " << choices.steps.size() << " steps:" << std::endl;

    for (const auto& step : choices.steps) {
        std::cout << "  Step: " << step.name << std::endl;
        for (const auto& group : step.groups) {
            std::cout << "    Group: " << group.name << std::endl;
            for (const auto& choice : group.choices) {
                std::cout << "      Choice: " << choice.name << " (idx=" << choice.idx << ")" << std::endl;
            }
        }
    }

    // Test the key fix - same group name "Read first" in different steps
    std::cout << std::endl << "Testing composite key fix:" << std::endl;

    // Step 1: "Read first" -> "Proceed"
    bool test1 = choices.isSelected("Installation Notice", "Read first", "Proceed");
    std::cout << "  Installation Notice / Read first / Proceed: "
              << (test1 ? "PASS" : "FAIL") << std::endl;

    // Step 2: "Read first" -> "Simplified" (same group name, different step)
    bool test2 = choices.isSelected("Choose Marker version", "Read first", "Simplified");
    std::cout << "  Choose Marker version / Read first / Simplified: "
              << (test2 ? "PASS" : "FAIL") << std::endl;

    // Make sure cross-contamination doesn't happen
    bool test3 = !choices.isSelected("Installation Notice", "Read first", "Simplified");
    std::cout << "  Installation Notice / Read first / Simplified (should be false): "
              << (test3 ? "PASS" : "FAIL") << std::endl;

    bool test4 = !choices.isSelected("Choose Marker version", "Read first", "Proceed");
    std::cout << "  Choose Marker version / Read first / Proceed (should be false): "
              << (test4 ? "PASS" : "FAIL") << std::endl;

    // Test getSelectedOptions
    std::cout << std::endl << "Testing getSelectedOptions:" << std::endl;
    auto opts1 = choices.getSelectedOptions("Installation Notice", "Read first");
    std::cout << "  Installation Notice / Read first: ";
    for (const auto& opt : opts1) std::cout << opt << " ";
    std::cout << std::endl;

    auto opts2 = choices.getSelectedOptions("Choose Marker version", "Read first");
    std::cout << "  Choose Marker version / Read first: ";
    for (const auto& opt : opts2) std::cout << opt << " ";
    std::cout << std::endl;

    auto opts3 = choices.getSelectedOptions("Choose Marker version", "Color Variation");
    std::cout << "  Choose Marker version / Color Variation: ";
    for (const auto& opt : opts3) std::cout << opt << " ";
    std::cout << std::endl;

    // Summary
    bool allPassed = test1 && test2 && test3 && test4;
    std::cout << std::endl << "=== " << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED")
              << " ===" << std::endl;

    return allPassed ? 0 : 1;
}
