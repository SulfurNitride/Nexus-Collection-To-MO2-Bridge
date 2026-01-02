#include "fomod.cpp"
#include "xml.cpp"
#include "json.cpp"
#include <iostream>
#include <fstream>

// Mock console to satisfy dependency if needed, or just rely on stouts
// fomod.cpp uses std::cout/cerr directly mostly or Console::log if updated?
// The version I wrote uses std::cout/cerr directly in fomod.cpp

int main() {
    // 1. Create a dummy FOMOD XML
    std::string xmlContent = R"(
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://q.....">
  <moduleName>Test Mod</moduleName>
  <installSteps>
    <step name="Installation Notice">
      <optionalFileGroups>
        <group name="Read first" type="SelectExactlyOne">
          <plugins>
            <plugin name="Proceed">
              <description>Go ahead.</description>
              <files>
                <file source="read_me.txt" destination="read_me.txt" priority="0"/>
              </files>
              <conditionFlags/>
              <typeDescriptor>
                <type name="Optional"/>
              </typeDescriptor>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </step>
    <step name="Choose Marker version">
      <optionalFileGroups>
        <group name="Read first" type="SelectExactlyOne">
          <plugins>
            <plugin name="Simplified">
              <description>Simple markers.</description>
              <files>
                <file source="simplified.esp" destination="simplified.esp" priority="0"/>
              </files>
              <conditionFlags/>
              <typeDescriptor>
                <type name="Optional"/>
              </typeDescriptor>
            </plugin>
          </plugins>
        </group>
        <group name="Color Variation" type="SelectExactlyOne">
          <plugins>
            <plugin name="Non colored Main Cities">
              <description>BW markers.</description>
              <files>
                <file source="bw.esp" destination="bw.esp" priority="0"/>
              </files>
              <conditionFlags/>
              <typeDescriptor>
                <type name="Optional"/>
              </typeDescriptor>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </step>
  </installSteps>
</config>
)";

    // 2. Create dummy files
    fs::create_directories("test_src/fomod");
    std::ofstream xmlFile("test_src/fomod/ModuleConfig.xml");
    xmlFile << xmlContent;
    xmlFile.close();

    std::ofstream f1("test_src/read_me.txt"); f1 << "hello"; f1.close();
    std::ofstream f2("test_src/simplified.esp"); f2 << "data"; f2.close();
    std::ofstream f3("test_src/bw.esp"); f3 << "data"; f3.close();

    // 3. Create dummy JSON Choices
    // Structure matching the one you provided
    std::string jsonChoices = R"({
        "options": [
          {
            "name": "Installation Notice",
            "groups": [
              {
                "name": "Read first",
                "choices": [
                  {
                    "name": "Proceed",
                    "idx": 0
                  }
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
                  {
                    "name": "Simplified",
                    "idx": 0
                  }
                ]
              },
              {
                "name": "Color Variation",
                "choices": [
                  {
                    "name": "Non colored Main Cities",
                    "idx": 1
                  }
                ]
              }
            ]
          }
        ]
    })";

    auto rootOpt = TinyJson::Parser::parse(jsonChoices);
    if (!rootOpt) {
        std::cerr << "JSON Parse Failed" << std::endl;
        return 1;
    }

    // 4. Run FomodParser
    fs::create_directories("test_dst");
    
    std::cout << "Running FOMOD Test..." << std::endl;
    bool res = FomodParser::process("test_src", "test_dst", *rootOpt);
    
    if (res) {
        std::cout << "FOMOD Process returned true." << std::endl;
        if (fs::exists("test_dst/read_me.txt")) std::cout << "PASS: read_me.txt installed." << std::endl;
        else std::cout << "FAIL: read_me.txt missing." << std::endl;

        if (fs::exists("test_dst/simplified.esp")) std::cout << "PASS: simplified.esp installed." << std::endl;
        else std::cout << "FAIL: simplified.esp missing." << std::endl;

        if (fs::exists("test_dst/bw.esp")) std::cout << "PASS: bw.esp installed." << std::endl;
        else std::cout << "FAIL: bw.esp missing." << std::endl;
        
    } else {
        std::cout << "FOMOD Process returned false." << std::endl;
    }

    // Cleanup
    fs::remove_all("test_src");
    fs::remove_all("test_dst");

    return 0;
}
