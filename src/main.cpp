#include "json.h"
#include "downloader.cpp"
#include "installer.cpp"
#include "fomod.cpp"
#include "api_client.cpp"
#include "console.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <regex>

namespace fs = std::filesystem;

std::string readFile(const std::string& path) {
    std::ifstream t(path);
    if (!t.is_open()) return "";
    std::stringstream buffer;
    buffer << t.rdbuf();
    return buffer.str();
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (std::string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

// Helper to URL Encode spaces and special chars
std::string urlEncode(const std::string &value) {
    std::ostringstream escaped;
    for (char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':' || c == '?' || c == '&' || c == '=') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::hex << ((int)c & 0xFF);
        }
    }
    return escaped.str();
}

bool fileExists(const std::string& path, long long expectedSize = 0) {
    if (!fs::exists(path)) return false;
    try {
        auto actual = fs::file_size(path);
        if (expectedSize > 0) {
            // Allow 1% variance or exact?
            if (actual != expectedSize) return false;
        }
        return actual > 0;
    } catch (...) {
        return false;
    }
}

std::string loadApiKey(const std::string& argKey) {
    if (!argKey.empty()) return argKey;
    if (fs::exists("nexus_apikey.txt")) return trim(readFile("nexus_apikey.txt"));
    return "";
}

std::map<int, std::string> buildModMap(const std::string& modsPath) {
    std::map<int, std::string> map;
    if (!fs::exists(modsPath)) return map;
    
    // Vortex Regex: Name-ModID-Version... 
    // use [0-9] to avoid backslash confusion
    std::regex vortexRegex("-([0-9]+)-");

    for (const auto& entry : fs::directory_iterator(modsPath)) {
        if (entry.is_directory()) {
            std::string folderName = entry.path().filename().string();
            int id = -1;

            // 1. Check MO2 meta.ini
            std::string metaPath = (entry.path() / "meta.ini").string();
            if (fs::exists(metaPath)) {
                std::ifstream meta(metaPath);
                std::string line;
                while (std::getline(meta, line)) {
                    if (line.find("modid=") == 0) {
                        try {
                            std::string val = line.substr(6);
                            val.erase(std::remove(val.begin(), val.end(), '\r'), val.end());
                            id = std::stoi(val);
                        } catch (...) {}
                        break; 
                    }
                }
            }

            // 2. If no meta.ini (or no ID found), try Vortex folder name parsing
            if (id == -1) {
                std::smatch match;
                if (std::regex_search(folderName, match, vortexRegex) && match.size() > 1) {
                    try {
                        id = std::stoi(match.str(1));
                    } catch (...) {}
                }
            }

            if (id != -1) {
                map[id] = folderName;
            }
        }
    }
    return map;
}

struct PluginEntry { std::string name; int position; };
bool comparePlugins(const PluginEntry& a, const PluginEntry& b) { return a.position < b.position; }

std::vector<std::string> findPluginsInMod(const std::string& modPath) {
    std::vector<std::string> plugins;
    if (!fs::exists(modPath)) return plugins;
    for (const auto& entry : fs::recursive_directory_iterator(modPath)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".esp" || ext == ".esm" || ext == ".esl") {
                plugins.push_back(entry.path().filename().string());
            }
        }
    }
    return plugins;
}

std::string fetchCollectionJson(const std::string& urlOrSlug, const std::string& apiKey) {
    if (apiKey.empty()) {
        Console::error("Error: API Key required to download collection from URL.");
        return "";
    }

    std::string slug = urlOrSlug;
    std::regex slugRegex("collections/([^/]+)");
    std::smatch match;
    if (std::regex_search(urlOrSlug, match, slugRegex) && match.size() > 1) {
        slug = match.str(1);
    }

    Console::log("Fetching collection metadata for slug: ", slug);

    std::string graphQL = "{ collection(slug: \"" + slug + "\") { latestPublishedRevision { downloadLink } } }";
    graphQL.erase(std::remove(graphQL.begin(), graphQL.end(), '\n'), graphQL.end());
    
    std::string url = "https://api.nexusmods.com/v2/graphql?query=" + urlEncode(graphQL);
    
    Console::log("DEBUG: Querying: ", url);

    std::string response = ApiClient::get(url, apiKey);
    auto rootOpt = TinyJson::Parser::parse(response);
    
    if (!rootOpt) {
         Console::error("Failed to parse GraphQL response.");
         return "";
    }
    
    std::string downloadLink;
    try {
        downloadLink = (*rootOpt)["data"]["collection"]["latestPublishedRevision"]["downloadLink"].asString();
    } catch(...) {}

    if (downloadLink.empty()) {
        Console::error("Failed to find download link in API response.");
        return "";
    }

    std::string fullApiUrl = "https://api.nexusmods.com" + downloadLink;
    Console::log("Resolving file URL from: ", fullApiUrl);
    
    std::string fileResponse = ApiClient::get(fullApiUrl, apiKey);
    auto fileRootOpt = TinyJson::Parser::parse(fileResponse);
    
    std::string fileUrl;
    if (fileRootOpt) {
        const auto& arr = (*fileRootOpt)["download_links"];
        if (arr.isArray() && arr.asArray().size() > 0) {
            fileUrl = arr[0]["URI"].asString();
        }
    }

    if (fileUrl.empty()) {
        Console::error("Failed to retrieve file URI.");
        return "";
    }

    Console::log("Downloading collection archive...");
    std::string tempArchive = "collection_temp.7z";
    
    CURL* curl = curl_easy_init();
    if (curl) {
        FILE* fp = fopen(tempArchive.c_str(), "wb");
        if (fp) {
            curl_easy_setopt(curl, CURLOPT_URL, fileUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, FILE* stream) {
                return fwrite(ptr, size, nmemb, stream);
            });
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexusBridge/1.0");
            
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                Console::error(std::string("Collection download failed: ") + curl_easy_strerror(res));
                fclose(fp);
                curl_easy_cleanup(curl);
                return "";
            }
            fclose(fp);
        } else {
            Console::error("Failed to create temp file.");
            curl_easy_cleanup(curl);
            return "";
        }
        curl_easy_cleanup(curl);
    } else {
        return "";
    }

    Console::log("Extracting collection...");
    Installer::extract(tempArchive, "collection_extracted");
    
    for (const auto& entry : fs::recursive_directory_iterator("collection_extracted")) {
        if (entry.path().extension() == ".json") {
             Console::log("Found JSON: ", entry.path().string());
             return readFile(entry.path().string());
        }
    }

    return "";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        Console::error("Usage: NexusBridge <collection.json OR URL> <mo2_base_path> [api_key]");
        return 1;
    }

    std::string inputPath = argv[1];
    std::string mo2Path = argv[2];
    std::string apiKey = loadApiKey((argc > 3) ? argv[3] : "");
    
    std::string downloadsDir = mo2Path + "/downloads";
    std::string modsDir = mo2Path + "/mods";
    std::string profilesDir = mo2Path + "/profiles/Default";

    fs::create_directories(downloadsDir);
    fs::create_directories(modsDir);

    std::string jsonContent;
    if (inputPath.find("http") == 0 || inputPath.find("nexusmods.com") != std::string::npos) {
        jsonContent = fetchCollectionJson(inputPath, apiKey);
    } else {
        Console::log("Loading Collection File: ", inputPath);
        jsonContent = readFile(inputPath);
    }

    if (jsonContent.empty()) {
        Console::error("Failed to load collection JSON.");
        return 1;
    }

    auto rootOpt = TinyJson::Parser::parse(jsonContent);
    if (!rootOpt) {
        Console::error("Failed to parse JSON");
        return 1;
    }
    const auto& root = *rootOpt;

    Downloader downloader(0, apiKey);

    Console::log("Mapping installed mods...");
    auto modMap = buildModMap(modsDir);

    // Helper for installation logic
    auto installFunc = [&](const std::string& archivePath, const std::string& modFolder, const TinyJson::Value& choices, int modId) {
        if (!fileExists(archivePath)) {
            Console::error("Install skipped, archive missing: ", archivePath);
            return;
        }
        std::string fullModPath = modsDir + "/" + modFolder;
        if (!fs::exists(fullModPath)) {
             if (Installer::install(archivePath, modsDir, modFolder)) {
                 // Pass fullModPath as both source and dest, assuming in-place processing or relative logic inside FomodParser
                 FomodParser::process(fullModPath, fullModPath, choices);
                 
                 // Write meta.ini for persistent ID mapping
                 if (modId != -1) {
                     std::ofstream meta(fullModPath + "/meta.ini");
                     meta << "[General]\nmodid=" << modId << "\n";
                     meta.close();
                     
                     // Also update the in-memory map immediately so plugin sorting can use it
                     modMap[modId] = modFolder;
                 }
             }
        } else {
            // Even if mod exists, ensure map is updated (in case buildModMap missed it or it was just installed)
            if (modId != -1) modMap[modId] = modFolder;
        }
    };

    Console::log("Starting Download & Install Phase...");
    if (root["mods"].isArray()) {
        for (const auto& mod : root["mods"].asArray()) {
            std::string filename = "unknown.7z";
            if (!mod["source"]["logicalFilename"].isNull()) filename = mod["source"]["logicalFilename"].asString();
            
            int modId = -1;
            if (mod["source"]["modId"].isNumber()) modId = mod["source"]["modId"].asInt();
            
            std::string modFolderName = filename; 
            size_t lastindex = filename.find_last_of("."); 
            if (lastindex != std::string::npos) modFolderName = filename.substr(0, lastindex);
            
            // If already mapped, use that folder name
            if (modId != -1 && modMap.count(modId)) modFolderName = modMap[modId];

            std::string installPath = modsDir + "/" + modFolderName;
            std::string archivePath = downloadsDir + "/" + filename;
            
            long long fileSize = 0;
            if (!mod["source"]["fileSize"].isNull()) {
                double d = mod["source"]["fileSize"].asNumber();
                fileSize = (long long)d;
            }

            if (!fileExists(archivePath, fileSize)) {
                if (!apiKey.empty() && modId != -1) {
                    int fileId = -1;
                    if (mod["source"]["fileId"].isNumber()) fileId = mod["source"]["fileId"].asInt();

                    if (fileId != -1) {
                        std::string linkUrl = "https://api.nexusmods.com/v1/games/skyrimspecialedition/mods/" + std::to_string(modId) + "/files/" + std::to_string(fileId) + "/download_link.json";
                        std::string linkJson = ApiClient::get(linkUrl, apiKey);
                        auto linkRoot = TinyJson::Parser::parse(linkJson);
                        
                        std::string downloadUri;
                        if (linkRoot && linkRoot->isArray() && linkRoot->asArray().size() > 0) {
                            downloadUri = (*linkRoot)[0]["URI"].asString();
                        }

                        if (!downloadUri.empty()) {
                            downloadUri.erase(0, downloadUri.find_first_not_of(" \t\n\r"));
                            downloadUri.erase(downloadUri.find_last_not_of(" \t\n\r") + 1);
                            downloadUri = urlEncode(downloadUri);

                            Console::log("[Queueing] ", filename, " -> ", downloadUri);
                            TinyJson::Value choices = mod["choices"];
                            downloader.addTask({downloadUri, archivePath, filename, fileId, fileSize, 
                                [=](const std::string& outPath) {
                                    installFunc(outPath, modFolderName, choices, modId);
                                }
                            });
                        } else {
                            Console::error("Failed to get download link for: ", filename);
                        }
                    }
                } else {
                     Console::log("[Missing] ", filename, " (No API Key or Mod ID)");
                }
            } else {
                downloader.addTask({"", archivePath, filename, 0, fileSize, 
                    [=](const std::string& outPath) {
                        installFunc(outPath, modFolderName, mod["choices"], modId);
                    }
                });
            }
        }
    }

    downloader.wait();
    
    Console::log("Generating plugins.txt (LOOT Order)...");
    std::vector<PluginEntry> pluginList;
    
    // Check for "plugins" array (Implicit Load Order)
    if (root["plugins"].isArray()) {
        const auto& pluginsArr = root["plugins"].asArray();
        Console::log("DEBUG: Found 'plugins' array with ", pluginsArr.size(), " entries.");
        
        for (size_t i = 0; i < pluginsArr.size(); ++i) {
            const auto& p = pluginsArr[i];
            std::string name = p["name"].asString();
            bool enabled = p["enabled"].asBool();
            
            if (!name.empty() && enabled) {
                // In this format, the name is the filename (e.g. "SkyUI.esp")
                // We just trust the order.
                pluginList.push_back({name, (int)i});
            }
        }
    }
    // Fallback/Legacy check for "loadOrder" object just in case
    else if (root["loadOrder"].isObject()) {
        const auto& lo = root["loadOrder"].asObject();
        Console::log("DEBUG: Found 'loadOrder' object with ", lo.size(), " entries.");
        
        for (const auto& [key, val] : lo) {
            int pos = -1;
            bool enabled = true;
            if (val.isObject()) {
                if (val["pos"].isNumber()) pos = val["pos"].asInt();
                if (!val["enabled"].isNull() && val["enabled"].isBool()) enabled = val["enabled"].asBool();
            }
            if (pos != -1 && enabled) {
                bool isModId = std::all_of(key.begin(), key.end(), ::isdigit);
                if (isModId) {
                    int id = std::stoi(key);
                    if (modMap.count(id)) {
                        std::string mPath = modsDir + "/" + modMap[id];
                        auto plugins = findPluginsInMod(mPath);
                        for (const auto& p : plugins) pluginList.push_back({p, pos});
                    }
                } else {
                    pluginList.push_back({key, pos});
                }
            }
        }
    } else {
        Console::error("DEBUG: Neither 'plugins' array nor 'loadOrder' object found.");
    }

    std::sort(pluginList.begin(), pluginList.end(), comparePlugins);
    Console::log("DEBUG: Total plugins to write: ", pluginList.size());
    
    std::string pluginsTxtPath = profilesDir + "/plugins.txt";
    std::ofstream pOut(pluginsTxtPath);
    pOut << "# Generated by NexusBridge" << std::endl;
    for (const auto& p : pluginList) pOut << "*" << p.name << std::endl;
    pOut.close();

    Console::log("Done! Please restart Mod Organizer 2.");
    return 0;
}
