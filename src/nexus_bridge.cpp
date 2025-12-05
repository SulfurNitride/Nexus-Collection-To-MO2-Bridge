/**
 * NexusBridge - Nexus Collections to MO2 Bridge
 *
 * Downloads and installs Nexus Collections directly to Mod Organizer 2 with:
 * - Direct downloads from Nexus (Premium required for CDN)
 * - Correct FOMOD option selections
 * - Proper mod load order (modlist.txt)
 * - Plugin load order (plugins.txt)
 *
 * NO Vortex installation required - fully independent.
 */

#include "../include/nlohmann/json.hpp"
#include "fomod_installer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
  #define NOMINMAX
  #include <windows.h>
#else
  #include <unistd.h>
  #include <climits>
#endif

// libloot for plugin sorting
#include <loot/api.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================================
// Utility Functions
// ============================================================================

std::string readFile(const std::string &path) {
  std::ifstream t(path);
  if (!t.is_open())
    return "";
  std::stringstream buffer;
  buffer << t.rdbuf();
  return buffer.str();
}

// Get directory containing this executable
fs::path getExecutableDir() {
  static fs::path dir;
  if (!dir.empty()) return dir;

#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    dir = fs::path(buf).parent_path();
    return dir;
  }
#else
  char buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len != -1) {
    buf[len] = '\0';
    dir = fs::path(buf).parent_path();
    return dir;
  }
#endif
  dir = fs::current_path();
  return dir;
}

// Get cross-platform temp directory
fs::path getTempDir() {
#ifdef _WIN32
  const char* temp = std::getenv("TEMP");
  if (temp) return fs::path(temp);
  temp = std::getenv("TMP");
  if (temp) return fs::path(temp);
  // Fallback to current directory
  return fs::current_path();
#else
  const char* temp = std::getenv("TMPDIR");
  if (temp) return fs::path(temp);
  return fs::path("/tmp");
#endif
}

std::string trim(const std::string &str) {
  size_t first = str.find_first_not_of(" \t\n\r");
  if (std::string::npos == first)
    return "";
  size_t last = str.find_last_not_of(" \t\n\r");
  return str.substr(first, (last - first + 1));
}

std::string urlEncode(const std::string &value) {
  std::ostringstream escaped;
  for (unsigned char c : value) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped << c;
    } else {
      escaped << '%' << std::uppercase << std::hex << std::setfill('0')
              << std::setw(2) << (int)c;
    }
  }
  return escaped.str();
}

// Encode spaces in URL path portion (before ?) to %20
// Nexus CDN returns URLs with spaces in filenames which curl can't handle
std::string encodeUrlSpaces(const std::string &url) {
  std::string result;
  result.reserve(url.size() + 10); // Reserve extra for %20s

  size_t queryPos = url.find('?');
  std::string path =
      (queryPos != std::string::npos) ? url.substr(0, queryPos) : url;
  std::string query =
      (queryPos != std::string::npos) ? url.substr(queryPos) : "";

  // Encode spaces in path only
  for (char c : path) {
    if (c == ' ') {
      result += "%20";
    } else {
      result += c;
    }
  }
  result += query;
  return result;
}

std::string loadApiKey(const std::string &argKey) {
  if (!argKey.empty())
    return argKey;
  // Check current directory first
  if (fs::exists("nexus_apikey.txt"))
    return trim(readFile("nexus_apikey.txt"));
  // Check TUI config directory (platform-specific)
#ifdef _WIN32
  const char* appData = std::getenv("APPDATA");
  if (appData) {
    fs::path configKey = fs::path(appData) / "NexusBridge" / "apikey.txt";
    if (fs::exists(configKey))
      return trim(readFile(configKey.string()));
  }
#else
  const char* home = std::getenv("HOME");
  if (home) {
    fs::path configKey = fs::path(home) / ".config" / "nexusbridge" / "apikey.txt";
    if (fs::exists(configKey))
      return trim(readFile(configKey.string()));
  }
#endif
  return "";
}

std::string get7zCommand() {
#ifdef _WIN32
  const std::string exeName = "7za.exe";
#else
  const std::string exeName = "7zzs";
#endif

  // First check in executable's directory
  fs::path exeDir = getExecutableDir();
  fs::path bundledPath = exeDir / exeName;
  if (fs::exists(bundledPath)) {
#ifndef _WIN32
    // Ensure it's executable on Linux
    std::string chmod = "chmod +x " + bundledPath.string();
    std::system(chmod.c_str());
#endif
    return bundledPath.string();
  }

  // Fallback: check current directory
  if (fs::exists(exeName)) {
#ifndef _WIN32
    std::string chmod = "chmod +x ./" + exeName;
    std::system(chmod.c_str());
#endif
    return "./" + exeName;
  }

  return "7z"; // Fallback to global
}

// ============================================================================
// Mod Information Structures
// ============================================================================

struct ModInfo {
  std::string name;
  std::string logicalFilename;
  int modId = -1;
  int fileId = -1;
  long long fileSize = 0;
  std::string md5;
  int phase = 0;
  json choices; // FOMOD choices if present
  std::string tag;
  std::string folderName; // Actual installed folder name
  std::string sourceType; // "nexus" or "direct"
  std::string directUrl;  // URL for direct downloads (non-Nexus)
  std::vector<std::string> expectedPaths; // Expected file paths from collection hashes
};

struct ModRule {
  std::string type;
  std::string sourceMd5;
  std::string sourceLogicalName;
  std::string referenceMd5;
  std::string referenceLogicalName;
};

struct PluginInfo {
  std::string name;
  bool enabled = true;
};

struct PluginRule {
  std::string name;
  std::vector<std::string> after;
};

// ============================================================================
// CURL Helpers with Progress
// ============================================================================

struct DownloadProgress {
  std::string filename;
  curl_off_t lastPrinted = 0;
};

static size_t WriteCallback(void *contents, size_t size, size_t nmemb,
                            std::string *userp) {
  userp->append(static_cast<const char *>(contents), size * nmemb);
  return size * nmemb;
}

static size_t WriteFileCallback(void *contents, size_t size, size_t nmemb,
                                FILE *userp) {
  return fwrite(contents, size, nmemb, userp);
}

static int ProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  if (dltotal <= 0)
    return 0;

  auto *prog = static_cast<DownloadProgress *>(clientp);

  // Only print every 5%
  int percent = static_cast<int>((dlnow * 100) / dltotal);
  int lastPercent = static_cast<int>((prog->lastPrinted * 100) / dltotal);

  if (percent >= lastPercent + 5 || dlnow == dltotal) {
    double dlMB = dlnow / (1024.0 * 1024.0);
    double totalMB = dltotal / (1024.0 * 1024.0);
    std::cout << "\r  Downloading: " << std::fixed << std::setprecision(1)
              << dlMB << " / " << totalMB << " MB (" << percent << "%)"
              << std::flush;
    prog->lastPrinted = dlnow;
  }

  return 0;
}

std::string httpGet(const std::string &url, const std::string &apiKey,
                    long *httpCode = nullptr, int maxRetries = 3) {
  std::string response;

  for (int attempt = 1; attempt <= maxRetries; ++attempt) {
    response.clear();
    CURL *curl = curl_easy_init();

    if (curl) {
      struct curl_slist *headers = nullptr;
      if (!apiKey.empty()) {
        headers = curl_slist_append(headers, ("apikey: " + apiKey).c_str());
      }
      headers = curl_slist_append(headers, "User-Agent: NexusBridge/2.0");

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);  // 60 second timeout
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);  // 30 second connect timeout

      CURLcode res = curl_easy_perform(curl);

      if (httpCode) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpCode);
      }

      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);

      if (res == CURLE_OK) {
        return response;  // Success
      }

      // Retry on timeout or connection errors
      if (res == CURLE_OPERATION_TIMEDOUT || res == CURLE_COULDNT_CONNECT ||
          res == CURLE_COULDNT_RESOLVE_HOST || res == CURLE_GOT_NOTHING) {
        if (attempt < maxRetries) {
          std::cerr << "  HTTP request failed (attempt " << attempt << "/" << maxRetries
                    << "): " << curl_easy_strerror(res) << " - retrying..." << std::endl;
          std::this_thread::sleep_for(std::chrono::seconds(2));
          continue;
        }
      }

      std::cerr << "  HTTP request failed: " << curl_easy_strerror(res)
                << std::endl;
      return response;  // Return empty on final failure
    }
  }

  return response;
}

bool downloadFile(const std::string &url, const std::string &destPath,
                  const std::string &filename = "",
                  long long expectedSize = 0) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return false;

  FILE *fp = fopen(destPath.c_str(), "wb");
  if (!fp) {
    curl_easy_cleanup(curl);
    return false;
  }

  DownloadProgress progress;
  progress.filename = filename;

  // Encode spaces in URL path (Nexus CDN returns filenames with spaces)
  std::string encodedUrl = encodeUrlSpaces(url);
  curl_easy_setopt(curl, CURLOPT_URL, encodedUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexusBridge/2.0");
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1000L); // 1KB/s minimum
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);    // for 60 seconds

  CURLcode res = curl_easy_perform(curl);
  std::cout << std::endl; // New line after progress

  fclose(fp);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    std::cerr << "  Download failed: " << curl_easy_strerror(res) << std::endl;
    fs::remove(destPath);
    return false;
  }

  // Verify file size if expected
  if (expectedSize > 0) {
    auto actualSize = fs::file_size(destPath);
    if (actualSize != static_cast<uintmax_t>(expectedSize)) {
      std::cerr << "  Size mismatch: expected " << expectedSize << ", got "
                << actualSize << std::endl;
      // Don't delete - partial download might be resumable
    }
  }

  return true;
}

// ============================================================================
// Nexus API Functions
// ============================================================================

class NexusAPI {
private:
  std::string apiKey;
  std::string gameDomain;

  // Rate limiting
  std::chrono::steady_clock::time_point lastRequest;

  void rateLimitWait() {
    // Nexus allows 30 requests/second for Premium, less for free
    // Be conservative - wait 100ms between requests
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastRequest);
    if (elapsed.count() < 100) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(100 - elapsed.count()));
    }
    lastRequest = std::chrono::steady_clock::now();
  }

public:
  bool isPremium = false;

  NexusAPI(const std::string &key, const std::string &game)
      : apiKey(key), gameDomain(game), lastRequest(std::chrono::steady_clock::now()) {
  }

  bool validateKey() {
    std::cout << "Validating Nexus API key..." << std::endl;

    long httpCode = 0;
    std::string response = httpGet(
        "https://api.nexusmods.com/v1/users/validate.json", apiKey, &httpCode);

    if (httpCode != 200 || response.empty()) {
      std::cerr << "API key validation failed (HTTP " << httpCode << ")"
                << std::endl;
      return false;
    }

    try {
      json data = json::parse(response);
      std::string username = data.value("name", "Unknown");
      isPremium = data.value("is_premium", false);

      std::cout << "  Logged in as: " << username << std::endl;
      std::cout << "  Premium: " << (isPremium ? "Yes" : "No") << std::endl;

      if (!isPremium) {
        std::cerr << std::endl;
        std::cerr
            << "WARNING: Premium membership required for direct downloads!"
            << std::endl;
        std::cerr
            << "Without Premium, you'll need to manually download via browser."
            << std::endl;
        std::cerr << std::endl;
      }

      return true;
    } catch (const json::exception &e) {
      std::cerr << "Failed to parse validation response: " << e.what()
                << std::endl;
      return false;
    }
  }

  // Get download links for a file
  // Returns empty vector if failed
  std::vector<std::string> getDownloadLinks(int modId, int fileId) {
    rateLimitWait();

    std::string url = "https://api.nexusmods.com/v1/games/" + gameDomain +
                      "/mods/" + std::to_string(modId) + "/files/" +
                      std::to_string(fileId) + "/download_link.json";

    long httpCode = 0;
    std::string response = httpGet(url, apiKey, &httpCode);

    std::vector<std::string> links;

    if (httpCode == 403) {
      // Premium required
      return links;
    }

    if (httpCode != 200 || response.empty()) {
      std::cerr << "  Failed to get download link (HTTP " << httpCode << ")"
                << std::endl;
      return links;
    }

    try {
      json data = json::parse(response);
      if (data.is_array()) {
        for (const auto &item : data) {
          if (item.contains("URI")) {
            links.push_back(item["URI"].get<std::string>());
          }
        }
      }
    } catch (const json::exception &e) {
      std::cerr << "  Failed to parse download links: " << e.what()
                << std::endl;
    }

    return links;
  }

  // Get file info (for filename, size verification)
  json getFileInfo(int modId, int fileId) {
    rateLimitWait();

    std::string url = "https://api.nexusmods.com/v1/games/" + gameDomain +
                      "/mods/" + std::to_string(modId) + "/files/" +
                      std::to_string(fileId) + ".json";

    std::string response = httpGet(url, apiKey);

    try {
      return json::parse(response);
    } catch (...) {
      return json();
    }
  }
};

// ============================================================================
// Collection Parser
// ============================================================================

class CollectionParser {
public:
  std::string collectionName;
  std::string author;
  std::string domainName;
  std::vector<ModInfo> mods;
  std::vector<ModRule> modRules;
  std::vector<PluginInfo> plugins;
  std::vector<PluginRule> pluginRules;

  bool parse(const std::string &jsonContent) {
    try {
      json root = json::parse(jsonContent);

      if (root.contains("info")) {
        const auto &info = root["info"];
        collectionName = info.value("name", "Unknown Collection");
        author = info.value("author", "Unknown");
        domainName = info.value("domainName", "skyrimspecialedition");
      }

      if (root.contains("mods") && root["mods"].is_array()) {
        for (const auto &modJson : root["mods"]) {
          ModInfo mod;
          mod.name = modJson.value("name", "");
          mod.phase = modJson.value("phase", 0);

          if (modJson.contains("source")) {
            const auto &src = modJson["source"];
            mod.modId = src.value("modId", -1);
            mod.fileId = src.value("fileId", -1);
            mod.fileSize = src.value("fileSize", 0LL);  // Use long long for files > 2GB
            mod.md5 = src.value("md5", "");
            mod.logicalFilename = src.value("logicalFilename", "");
            mod.tag = src.value("tag", "");
            mod.sourceType = src.value("type", "nexus");
            mod.directUrl = src.value("url", "");
          }

          if (modJson.contains("choices")) {
            mod.choices = modJson["choices"];
          }

          // Extract expected file paths from hashes (for hash-based installation)
          if (modJson.contains("hashes") && modJson["hashes"].is_array()) {
            for (const auto &hash : modJson["hashes"]) {
              if (hash.contains("path")) {
                // Normalize path separators (Windows uses backslash in collection.json)
                std::string path = hash["path"].get<std::string>();
                std::replace(path.begin(), path.end(), '\\', '/');
                mod.expectedPaths.push_back(path);
              }
            }
          }

          mods.push_back(mod);
        }
      }

      if (root.contains("modRules") && root["modRules"].is_array()) {
        for (const auto &ruleJson : root["modRules"]) {
          ModRule rule;
          rule.type = ruleJson.value("type", "");

          if (ruleJson.contains("source")) {
            rule.sourceMd5 = ruleJson["source"].value("fileMD5", "");
            rule.sourceLogicalName =
                ruleJson["source"].value("logicalFileName", "");
          }
          if (ruleJson.contains("reference")) {
            rule.referenceMd5 = ruleJson["reference"].value("fileMD5", "");
            rule.referenceLogicalName =
                ruleJson["reference"].value("logicalFileName", "");
          }

          modRules.push_back(rule);
        }
      }

      if (root.contains("plugins") && root["plugins"].is_array()) {
        for (const auto &pluginJson : root["plugins"]) {
          PluginInfo plugin;
          plugin.name = pluginJson.value("name", "");
          plugin.enabled = pluginJson.value("enabled", true);
          plugins.push_back(plugin);
        }
      }

      if (root.contains("pluginRules") && root["pluginRules"].is_object()) {
        const auto &pr = root["pluginRules"];
        if (pr.contains("plugins") && pr["plugins"].is_array()) {
          for (const auto &prJson : pr["plugins"]) {
            PluginRule rule;
            rule.name = prJson.value("name", "");
            if (prJson.contains("after") && prJson["after"].is_array()) {
              for (const auto &after : prJson["after"]) {
                rule.after.push_back(after.get<std::string>());
              }
            }
            pluginRules.push_back(rule);
          }
        }
      }

      std::cout << "Parsed collection: " << collectionName << " by " << author
                << std::endl;
      std::cout << "  Game: " << domainName << std::endl;
      std::cout << "  Mods: " << mods.size() << std::endl;
      std::cout << "  Mod Rules: " << modRules.size() << std::endl;
      std::cout << "  Plugins: " << plugins.size() << std::endl;

      return true;
    } catch (const json::exception &e) {
      std::cerr << "JSON parse error: " << e.what() << std::endl;
      return false;
    }
  }
};

// ============================================================================
// Archive Extraction
// ============================================================================

bool extractArchive(const std::string &archivePath,
                    const std::string &destPath) {
  fs::create_directories(destPath);

  std::string ext = fs::path(archivePath).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  std::string cmd;
  if (ext == ".7z" || ext == ".zip" || ext == ".rar") {
    // Use 7z for all supported formats
#ifdef _WIN32
    cmd = "\"" + get7zCommand() + "\" x -y -o\"" + destPath + "\" \"" + archivePath +
          "\" > NUL 2>&1";
#else
    cmd = get7zCommand() + " x -y -o\"" + destPath + "\" \"" + archivePath +
          "\" > /dev/null 2>&1";
#endif
  } else {
    std::cerr << "  Unsupported archive format: " << ext << std::endl;
    return false;
  }

  int result = system(cmd.c_str());
  return result == 0;
}

// Fix Windows backslash paths in extracted files
// Some archives created on Windows contain paths like "SKSE\Plugins\file.dll"
// which 7z on Linux extracts as literal filenames with backslashes
void fixWindowsBackslashPaths(const std::string &extractedPath) {
  std::vector<fs::path> toFix;

  // Collect files with backslashes in their names
  for (const auto &entry : fs::recursive_directory_iterator(extractedPath)) {
    if (entry.is_regular_file()) {
      std::string filename = entry.path().filename().string();
      if (filename.find('\\') != std::string::npos) {
        toFix.push_back(entry.path());
      }
    }
  }

  // Process each file with backslash paths
  for (const auto &filePath : toFix) {
    std::string filename = filePath.filename().string();
    fs::path parentDir = filePath.parent_path();

    // Replace backslashes with forward slashes to get proper path
    std::string fixedPath = filename;
    std::replace(fixedPath.begin(), fixedPath.end(), '\\', '/');

    fs::path destPath = parentDir / fixedPath;

    try {
      // Create destination directory structure
      fs::create_directories(destPath.parent_path());

      // Move file to correct location
      fs::rename(filePath, destPath);
    } catch (const std::exception &e) {
      std::cerr << "  [WARN] Failed to fix backslash path: " << filename
                << " -> " << fixedPath << " (" << e.what() << ")" << std::endl;
    }
  }
}

// Known Skyrim data folders that should NOT be treated as wrappers
static bool isDataFolder(const std::string &name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  static const std::set<std::string> dataFolders = {
      "meshes",       "textures",
      "scripts",      "sound",
      "interface",    "strings",
      "seq",          "grass",
      "video",        "music",
      "shaders",      "shadersfx",    "lodsettings",
      "skse",         "netscriptframework",
      "edit scripts", "dialogueviews",
      "facegen",      "caliente tools",
      "actors",       "fonts",
      "materials",    "platform",
      "source",       "terrain",
      "trees",        "vis",
      "distantlod",   "lod",
      "dyndolod",     "nemesis_engine"};

  return dataFolders.count(lower) > 0;
}

// Check if folder is the game's "Data" folder (should be unwrapped)
static bool isGameDataFolder(const std::string &name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lower == "data";
}

// Check if file is "junk" (readme, license, etc.) that shouldn't prevent
// unwrapping
static bool isJunkFile(const std::string &name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Extensions to ignore
  static const std::set<std::string> junkExts = {
      ".txt", ".md",  ".pdf", ".doc",  ".docx", ".rtf", ".url",
      ".ini", ".png", ".jpg", ".jpeg", ".bmp",  ".gif"};

  // Filenames to ignore (exact match)
  static const std::set<std::string> junkNames = {
      "readme",  "license", "changelog",   "credits",
      "authors", "install", "instructions"};

  // Check extension
  size_t dotPos = lower.find_last_of('.');
  if (dotPos != std::string::npos) {
    std::string ext = lower.substr(dotPos);
    if (junkExts.count(ext))
      return true;
  }

  // Check filename (contains junk name)
  for (const auto &junk : junkNames) {
    if (lower.find(junk) != std::string::npos)
      return true;
  }

  return false;
}

// Detect wrapper folder (single folder containing all mod content)
// Recursively unwraps version folders and "Data" folders
// Ignores junk files when determining if a folder is a wrapper
std::string detectWrapperFolder(const std::string &extractedPath) {
  std::string currentPath = extractedPath;

  // Keep unwrapping until we find actual content
  while (true) {
    std::vector<fs::path> dirs;
    std::vector<fs::path> files;

    for (const auto &entry : fs::directory_iterator(currentPath)) {
      if (entry.is_directory()) {
        dirs.push_back(entry.path());
      } else {
        files.push_back(entry.path());
      }
    }

    // If exactly one directory
    if (dirs.size() == 1) {
      bool hasSignificantFiles = false;
      for (const auto &f : files) {
        if (!isJunkFile(f.filename().string())) {
          hasSignificantFiles = true;
          break;
        }
      }

      // If no significant files alongside the directory, unwrap it
      if (!hasSignificantFiles) {
        std::string folderName = dirs[0].filename().string();

        // "Data" folder should be unwrapped (it's the game data folder)
        if (isGameDataFolder(folderName)) {
          currentPath = dirs[0].string();
          continue; // Keep unwrapping
        }

        // Don't unwrap if it's a known mod data folder (meshes, textures, etc.)
        if (isDataFolder(folderName)) {
          return currentPath;
        }

        // Otherwise it's a version wrapper - unwrap it
        currentPath = dirs[0].string();
        continue; // Keep unwrapping
      }
    }

    // Multiple directories or significant files - stop here
    return currentPath;
  }
}

// Find existing folder with case-insensitive match in destination
static fs::path findExistingFolder(const fs::path &destDir,
                                   const std::string &folderName) {
  if (!fs::exists(destDir) || !fs::is_directory(destDir)) {
    return fs::path();
  }

  std::string nameLower = folderName;
  std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  for (const auto &entry : fs::directory_iterator(destDir)) {
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
static void copyDirMerge(const fs::path &src, const fs::path &dst) {
  if (!fs::exists(dst)) {
    fs::create_directories(dst);
  }

  for (const auto &entry : fs::directory_iterator(src)) {
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

// Flatten "Data" folder if it exists in the root
// Moves contents of Data/ to root/ and removes Data/
void flattenDataFolder(const std::string &modRoot) {
  fs::path root(modRoot);
  fs::path dataPath;

  // Find "Data" folder case-insensitively
  for (const auto &entry : fs::directory_iterator(root)) {
    if (entry.is_directory() &&
        isGameDataFolder(entry.path().filename().string())) {
      dataPath = entry.path();
      break;
    }
  }

  if (dataPath.empty())
    return;

  std::cout << "    Flattening Data folder: " << dataPath.filename()
            << std::endl;

  // Move everything from Data/ to root/
  for (const auto &entry : fs::directory_iterator(dataPath)) {
    fs::path src = entry.path();
    fs::path dst = root / src.filename();

    try {
      if (fs::exists(dst)) {
        if (fs::is_directory(src) && fs::is_directory(dst)) {
          // Merge directories
          copyDirMerge(src, dst);
          fs::remove_all(src);
        } else {
          // Overwrite files (or rename if conflict? Overwrite is standard for
          // Data install)
          fs::rename(src, dst);
        }
      } else {
        fs::rename(src, dst);
      }
    } catch (const std::exception &e) {
      std::cerr << "    [WARN] Failed to move " << src.filename() << ": "
                << e.what() << std::endl;
    }
  }

  // Remove empty Data folder
  try {
    fs::remove(dataPath);
  } catch (...) {
  }
}

// Select variant folder based on mod name (for mods without FOMOD)
// When archive has multiple variant folders like "Mod - Option A" and "Mod - Option B",
// and mod name is "Mod - Option A", we pick that folder and flatten it
std::string selectVariantFolder(const std::string &contentPath,
                                 const std::string &modName) {
  std::vector<fs::path> dirs;
  std::vector<fs::path> files;

  for (const auto &entry : fs::directory_iterator(contentPath)) {
    if (entry.is_directory()) {
      dirs.push_back(entry.path());
    } else if (!isJunkFile(entry.path().filename().string())) {
      files.push_back(entry.path());
    }
  }

  // Only process if there are multiple directories and no significant files
  if (dirs.size() <= 1 || !files.empty()) {
    return contentPath;
  }

  // Normalize mod name for comparison
  std::string modNameLower = modName;
  std::transform(modNameLower.begin(), modNameLower.end(), modNameLower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Look for a folder that matches the mod name
  for (const auto &dir : dirs) {
    std::string folderName = dir.filename().string();
    std::string folderLower = folderName;
    std::transform(folderLower.begin(), folderLower.end(), folderLower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (folderLower == modNameLower) {
      std::cout << "    Selected variant folder: " << folderName << std::endl;
      return dir.string();
    }
  }

  return contentPath;
}

// ============================================================================
// Thread Pool for Parallel Installation
// ============================================================================

class ThreadPool {
public:
  explicit ThreadPool(size_t numThreads) : stop(false) {
    for (size_t i = 0; i < numThreads; ++i) {
      workers.emplace_back([this] {
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(queueMutex);
            condition.wait(lock, [this] { return stop || !tasks.empty(); });
            if (stop && tasks.empty())
              return;
            task = std::move(tasks.front());
            tasks.pop();
          }
          task();
        }
      });
    }
  }

  template <class F> void enqueue(F &&f) {
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      tasks.emplace(std::forward<F>(f));
    }
    condition.notify_one();
  }

  void wait() {
    // Wait for all tasks to complete
    while (true) {
      {
        std::unique_lock<std::mutex> lock(queueMutex);
        if (tasks.empty())
          break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Give threads time to finish current tasks
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  ~ThreadPool() {
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      stop = true;
    }
    condition.notify_all();
    for (auto &worker : workers) {
      if (worker.joinable())
        worker.join();
    }
  }

private:
  std::vector<std::thread> workers;
  std::queue<std::function<void()>> tasks;
  std::mutex queueMutex;
  std::condition_variable condition;
  bool stop;
};

// Download task for parallel downloading
struct DownloadTask {
  std::string url;
  std::string destPath;
  std::string filename;
  std::string modName;
  long long fileSize;
  int modId;
  int fileId;
  bool isDirectDownload;
  size_t modIndex;  // Index into collection.mods
};

// Install task for parallel processing
struct InstallTask {
  std::string archivePath;
  std::string destModPath;
  std::string tempDir;
  std::string folderName;
  std::string modName;
  json choices;
  size_t index;
  size_t total;
  std::vector<std::string> expectedPaths; // Expected files from collection hashes
};

// Global counters for thread-safe progress
static std::atomic<int> g_installed{0};
static std::atomic<int> g_failed{0};
static std::mutex g_printMutex;

// Thread-safe print
void safePrint(const std::string &msg) {
  std::lock_guard<std::mutex> lock(g_printMutex);
  std::cout << msg << std::flush;
}

// Install a single mod (can be called from thread pool)
bool installMod(const InstallTask &task) {
  std::string extractPath =
      task.tempDir + "/" + task.folderName + "_" + std::to_string(task.index);

  try {
    // Cleanup any existing extraction
    if (fs::exists(extractPath)) {
      fs::remove_all(extractPath);
    }

    // Extract archive
    if (!extractArchive(task.archivePath, extractPath)) {
      safePrint("  [" + std::to_string(task.index + 1) + "/" +
                std::to_string(task.total) + "] " + task.modName +
                " - FAILED: Extraction failed\n");
      g_failed++;
      return false;
    }

    // Fix Windows backslash paths (e.g., "SKSE\Plugins\file.dll" -> "SKSE/Plugins/file.dll")
    fixWindowsBackslashPaths(extractPath);

    // DEBUG: List extracted files
    if (task.modName.find("Animated Armoury") != std::string::npos ||
        task.modName.find("College of Winterhold") != std::string::npos) {
      safePrint("  [DEBUG] Extracted contents for " + task.modName + ":\n");
      for (const auto &entry : fs::recursive_directory_iterator(extractPath)) {
        safePrint("    " + entry.path().string() + "\n");
      }
    }

    // Handle wrapper folders
    std::string actualContent = detectWrapperFolder(extractPath);

    // DEBUG: Show what detectWrapperFolder returned
    if (task.modName.find("Cougar") != std::string::npos) {
      safePrint("  [DEBUG " + task.modName + "] extractPath: " + extractPath + "\n");
      safePrint("  [DEBUG " + task.modName + "] actualContent: " + actualContent + "\n");
    }

    // Check for FOMOD
    fs::path fomodXml = FomodInstaller::findModuleConfig(actualContent);

    if (!fomodXml.empty() && task.choices.contains("options")) {
      // FOMOD with explicit choices from collection
      FomodInstaller::FomodChoices choices =
          FomodInstaller::parseChoices(task.choices);
      if (!FomodInstaller::process(actualContent, task.destModPath, choices)) {
        // FOMOD had issues but may have partially worked
      }
    } else if (!fomodXml.empty() && !task.expectedPaths.empty()) {
      // FOMOD without choices but we have expected file paths from collection hashes
      // Use hash-based installation: find expected files in archive and copy them
      fs::create_directories(task.destModPath);

      // Build a case-insensitive map of files in the extracted archive
      std::map<std::string, fs::path> archiveFiles;
      for (const auto &entry : fs::recursive_directory_iterator(actualContent)) {
        if (entry.is_regular_file()) {
          // Get relative path from actualContent
          std::string relPath = fs::relative(entry.path(), actualContent).string();
          // Normalize to forward slashes and lowercase for matching
          std::replace(relPath.begin(), relPath.end(), '\\', '/');
          std::string lowerPath = relPath;
          std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
          archiveFiles[lowerPath] = entry.path();
        }
      }

      int copiedCount = 0;
      for (const std::string &expectedPath : task.expectedPaths) {
        // Normalize expected path for case-insensitive matching
        std::string lowerExpected = expectedPath;
        std::transform(lowerExpected.begin(), lowerExpected.end(), lowerExpected.begin(), ::tolower);

        // Try to find the file in the archive (case-insensitive)
        fs::path sourcePath;
        if (archiveFiles.count(lowerExpected)) {
          sourcePath = archiveFiles[lowerExpected];
        } else {
          // Try searching within subfolders (FOMOD installers often have files in subfolders)
          for (const auto &[archivePath, fullPath] : archiveFiles) {
            // Check if archivePath ends with expectedPath
            if (archivePath.length() >= lowerExpected.length() &&
                archivePath.substr(archivePath.length() - lowerExpected.length()) == lowerExpected) {
              sourcePath = fullPath;
              break;
            }
          }
        }

        if (!sourcePath.empty() && fs::exists(sourcePath)) {
          fs::path destPath = fs::path(task.destModPath) / expectedPath;
          fs::create_directories(destPath.parent_path());
          fs::copy_file(sourcePath, destPath, fs::copy_options::overwrite_existing);
          copiedCount++;
        }
      }

      if (copiedCount == 0) {
        // Hash-based install failed, fall back to standard copy
        safePrint("  [WARN] Hash-based install found 0 files for " + task.modName + ", falling back to standard\n");
        std::string installFrom = selectVariantFolder(actualContent, task.modName);
        for (const auto &entry : fs::directory_iterator(installFrom)) {
          fs::copy(entry.path(), fs::path(task.destModPath) / entry.path().filename(),
                   fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
      }
    } else {
      // Standard install - check for variant folder selection first
      std::string installFrom = selectVariantFolder(actualContent, task.modName);

      // DEBUG: Show what selectVariantFolder returned
      if (task.modName.find("Cougar") != std::string::npos) {
        safePrint("  [DEBUG " + task.modName + "] installFrom: " + installFrom + "\n");
        safePrint("  [DEBUG " + task.modName + "] destModPath: " + task.destModPath + "\n");
      }

      fs::create_directories(task.destModPath);

      // Count source files for verification
      int sourceFileCount = 0;
      for (const auto &e : fs::recursive_directory_iterator(installFrom)) {
        if (e.is_regular_file()) sourceFileCount++;
      }

      // Copy files
      for (const auto &entry : fs::directory_iterator(installFrom)) {
        fs::copy(
            entry.path(), fs::path(task.destModPath) / entry.path().filename(),
            fs::copy_options::recursive | fs::copy_options::overwrite_existing);
      }

      // Verify destination file count
      int destFileCount = 0;
      for (const auto &e : fs::recursive_directory_iterator(task.destModPath)) {
        if (e.is_regular_file()) destFileCount++;
      }

      // If truncated, retry with manual recursive copy
      if (destFileCount < sourceFileCount) {
        safePrint("  [WARN] Copy incomplete for " + task.modName +
                  " (" + std::to_string(destFileCount) + "/" +
                  std::to_string(sourceFileCount) + " files). Retrying...\n");

        // Clear destination and retry with explicit recursive copy
        fs::remove_all(task.destModPath);
        fs::create_directories(task.destModPath);

        // Manual recursive copy with error catching
        try {
            for (const auto& dirEntry : fs::recursive_directory_iterator(installFrom)) {
                if (dirEntry.is_regular_file()) {
                    fs::path relPath = fs::relative(dirEntry.path(), fs::path(installFrom));
                    fs::path targetPath = fs::path(task.destModPath) / relPath;
                    fs::create_directories(targetPath.parent_path());
                    fs::copy_file(dirEntry.path(), targetPath, fs::copy_options::overwrite_existing);
                }
            }
        } catch (const std::exception& e) {
            safePrint("  [ERROR] Manual copy failed: " + std::string(e.what()) + "\n");
        }

        // Re-verify
        destFileCount = 0;
        for (const auto &e : fs::recursive_directory_iterator(task.destModPath)) {
          if (e.is_regular_file()) destFileCount++;
        }

        if (destFileCount < sourceFileCount) {
          safePrint("  [ERROR] Copy still incomplete after retry for " + task.modName +
                    " (" + std::to_string(destFileCount) + "/" +
                    std::to_string(sourceFileCount) + " files)\n");
        }
      }
    }

    // Ensure Data folder is flattened (match Vortex structure)
    flattenDataFolder(task.destModPath);

    // Cleanup
    fs::remove_all(extractPath);

    g_installed++;
    safePrint("  [" + std::to_string(task.index + 1) + "/" +
              std::to_string(task.total) + "] " + task.modName + " - Done!\n");
    return true;

  } catch (const std::exception &e) {
    safePrint("  [" + std::to_string(task.index + 1) + "/" +
              std::to_string(task.total) + "] " + task.modName +
              " - FAILED: " + std::string(e.what()) + "\n");
    g_failed++;
    if (fs::exists(extractPath)) {
      try {
        fs::remove_all(extractPath);
      } catch (...) {
      }
    }
    return false;
  }
}

// ============================================================================
// Mod List Generator (modlist.txt)
// ============================================================================

class ModListGenerator {
public:
  // Topological sort using Kahn's algorithm
  // Returns mods sorted from lowest to highest priority (as folder names)
  static std::vector<std::string>
  generateModOrder(const std::vector<ModInfo> &mods,
                   const std::vector<ModRule> &rules) {
    // Build lookup maps:
    // - logicalNameToIdx: logicalFilename -> index (for rule lookups, since rules use logicalFileName)
    // - md5ToLogicalName: md5 -> logicalFilename (for MD5-based rule lookups)
    // - modFolders: index -> folderName (for output, since folderName is unique)
    std::map<std::string, int> logicalNameToIdx;
    std::map<std::string, std::string> md5ToLogicalName;
    std::vector<std::string> modFolders;

    for (size_t i = 0; i < mods.size(); ++i) {
      // Use logicalFilename as the key (it's what rules reference and is unique)
      // Fall back to name if logicalFilename is empty
      std::string key = mods[i].logicalFilename.empty() ? mods[i].name : mods[i].logicalFilename;
      logicalNameToIdx[key] = i;

      // Store folderName for output (unique identifier for modlist.txt)
      // Fall back to name if folderName not set
      std::string folder = mods[i].folderName.empty() ? mods[i].name : mods[i].folderName;
      modFolders.push_back(folder);

      if (!mods[i].md5.empty()) {
        md5ToLogicalName[mods[i].md5] = key;
      }
    }

    // Build adjacency list for forward edges (successors) and reverse edges (predecessors)
    // Edge A -> B means "A should come before B" (A has lower priority than B)
    std::vector<std::vector<int>> successors(mods.size());  // Forward edges: who comes after me
    std::vector<std::vector<int>> predecessors(mods.size()); // Reverse edges: who comes before me

    int appliedRules = 0;
    for (const auto &rule : rules) {
      // Find source mod (the mod the rule is on) by logicalFileName
      std::string srcKey = rule.sourceLogicalName;
      if (srcKey.empty() && !rule.sourceMd5.empty()) {
        auto it = md5ToLogicalName.find(rule.sourceMd5);
        if (it != md5ToLogicalName.end()) srcKey = it->second;
      }

      // Find reference mod (the mod being referenced) by logicalFileName
      std::string refKey = rule.referenceLogicalName;
      if (refKey.empty() && !rule.referenceMd5.empty()) {
        auto it = md5ToLogicalName.find(rule.referenceMd5);
        if (it != md5ToLogicalName.end()) refKey = it->second;
      }

      // Skip if we can't find either mod
      if (logicalNameToIdx.find(srcKey) == logicalNameToIdx.end() ||
          logicalNameToIdx.find(refKey) == logicalNameToIdx.end()) {
        continue;
      }

      int srcIdx = logicalNameToIdx[srcKey];
      int refIdx = logicalNameToIdx[refKey];

      if (rule.type == "before") {
        // source before reference: source has lower priority
        // Edge: source -> reference (source is predecessor of reference)
        successors[srcIdx].push_back(refIdx);
        predecessors[refIdx].push_back(srcIdx);
        appliedRules++;
      } else if (rule.type == "after") {
        // source after reference: source has higher priority
        // Edge: reference -> source (reference is predecessor of source)
        successors[refIdx].push_back(srcIdx);
        predecessors[srcIdx].push_back(refIdx);
        appliedRules++;
      }
    }

    std::cout << "  Applied " << appliedRules << " mod rules for sorting" << std::endl;

    // DFS-based topological sort (matches Vortex's graphlib.alg.topsort behavior)
    // graphlib starts from SINKS (nodes with no outgoing edges) and walks backwards
    // via predecessors, adding nodes in post-order
    std::vector<int> visited(mods.size(), 0);  // 0=unvisited, 1=in-progress, 2=done
    std::vector<std::string> sorted;
    sorted.reserve(mods.size());
    bool hasCycle = false;

    // DFS visit function (iterative to avoid stack overflow on large graphs)
    auto visit = [&](int start) {
      std::vector<std::pair<int, size_t>> stack;  // (node, next_predecessor_index)
      stack.push_back({start, 0});

      while (!stack.empty()) {
        auto& [node, predIdx] = stack.back();

        if (predIdx == 0) {
          // First time visiting this node
          if (visited[node] == 2) {
            stack.pop_back();
            continue;
          }
          if (visited[node] == 1) {
            // Cycle detected
            hasCycle = true;
            stack.pop_back();
            continue;
          }
          visited[node] = 1;  // Mark as in-progress
        }

        // Try to visit unvisited predecessors
        bool pushedPred = false;
        while (predIdx < predecessors[node].size()) {
          int pred = predecessors[node][predIdx];
          predIdx++;
          if (visited[pred] == 0) {
            stack.push_back({pred, 0});
            pushedPred = true;
            break;  // Process this predecessor first
          } else if (visited[pred] == 1) {
            hasCycle = true;
          }
        }

        // Only finish this node if we didn't push a predecessor
        // and we've processed all predecessors
        if (!pushedPred && predIdx >= predecessors[node].size()) {
          visited[node] = 2;
          sorted.push_back(modFolders[node]);
          stack.pop_back();
        }
      }
    };

    // Find sinks (nodes with no successors) and process them
    std::vector<int> sinks;
    for (size_t i = 0; i < mods.size(); ++i) {
      if (successors[i].empty()) {
        sinks.push_back(i);
      }
    }

    // Sort sinks ALPHABETICALLY by folder name for deterministic tie-breaking
    // This produces consistent, predictable ordering for mods without dependencies
    std::sort(sinks.begin(), sinks.end(), [&modFolders](int a, int b) {
      return modFolders[a] < modFolders[b];
    });

    for (int sink : sinks) {
      if (visited[sink] == 0) {
        visit(sink);
      }
    }

    // Visit any remaining unvisited nodes (handles disconnected components)
    // Sort alphabetically for consistent tie-breaking
    std::vector<int> remaining;
    for (size_t i = 0; i < mods.size(); ++i) {
      if (visited[i] == 0) {
        remaining.push_back(i);
      }
    }
    std::sort(remaining.begin(), remaining.end(), [&modFolders](int a, int b) {
      return modFolders[a] < modFolders[b];
    });
    for (int node : remaining) {
      if (visited[node] == 0) {
        visit(node);
      }
    }

    if (hasCycle) {
      std::cerr << "  [WARN] Cycle detected in mod rules, some mods may be misordered" << std::endl;
    }

    // DFS from sinks with post-order: predecessors added before dependents
    // First in sorted = sources (lowest priority = bottom of modlist)
    // Last in sorted = sinks (highest priority = top of modlist)
    // MO2: TOP = WINS. So we reverse to put sinks (highest priority) at top.
    std::reverse(sorted.begin(), sorted.end());
    return sorted;
  }

  // Fallback for when no rules are provided
  static std::vector<std::string>
  generateModOrder(const std::vector<ModInfo> &mods) {
    std::vector<ModRule> emptyRules;
    return generateModOrder(mods, emptyRules);
  }

  // modOrder now contains folder names directly (no lookup needed)
  static void
  writeModList(const std::string &path,
               const std::vector<std::string> &modOrder) {
    std::ofstream out(path);
    out << "# This file was automatically generated by NexusBridge"
        << std::endl;
    out << "# Mod priority: Top = Winner, Bottom = Loser" << std::endl;

    for (const auto &folderName : modOrder) {
      out << "+" << folderName << std::endl;
    }

    out.close();
    std::cout << "Generated modlist.txt with " << modOrder.size() << " mods"
              << std::endl;
  }

  // Build plugin position map from sorted plugins
  static std::map<std::string, int> buildPluginPositionMap(
      const std::vector<std::string> &sortedPlugins) {
    std::map<std::string, int> pluginPosition;
    for (size_t i = 0; i < sortedPlugins.size(); ++i) {
      std::string pluginLower = sortedPlugins[i];
      std::transform(pluginLower.begin(), pluginLower.end(), pluginLower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      pluginPosition[pluginLower] = i;
    }
    return pluginPosition;
  }

  // Get the earliest plugin position for a mod folder
  static int getModPluginPosition(const std::string &modFolder,
                                   const std::string &modsDir,
                                   const std::map<std::string, int> &pluginPosition) {
    fs::path modPath = fs::path(modsDir) / modFolder;
    if (!fs::exists(modPath)) return INT_MAX;

    int earliestPos = INT_MAX;
    try {
      for (const auto &entry : fs::recursive_directory_iterator(modPath)) {
        if (!entry.is_regular_file()) continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (ext == ".esp" || ext == ".esm" || ext == ".esl") {
          std::string pluginLower = entry.path().filename().string();
          std::transform(pluginLower.begin(), pluginLower.end(), pluginLower.begin(),
                         [](unsigned char c) { return std::tolower(c); });

          auto it = pluginPosition.find(pluginLower);
          if (it != pluginPosition.end()) {
            earliestPos = std::min(earliestPos, it->second);
          }
        }
      }
    } catch (...) {}
    return earliestPos;
  }

  // Kahn's algorithm for topological sort with tie-breaking
  // Returns indices in topological order (respects before/after constraints)
  static std::vector<int> kahnSort(
      size_t n,
      const std::vector<std::vector<int>> &successors,
      const std::vector<std::vector<int>> &predecessors,
      const std::vector<int> &tieBreaker) {  // Lower tieBreaker value = earlier in output

    std::vector<int> inDegree(n, 0);
    for (size_t i = 0; i < n; ++i) {
      inDegree[i] = predecessors[i].size();
    }

    // Priority queue: sort by tieBreaker (lower = higher priority to process first)
    auto cmp = [&](int a, int b) { return tieBreaker[a] > tieBreaker[b]; };
    std::priority_queue<int, std::vector<int>, decltype(cmp)> ready(cmp);

    for (size_t i = 0; i < n; ++i) {
      if (inDegree[i] == 0) {
        ready.push(i);
      }
    }

    std::vector<int> result;
    result.reserve(n);

    while (!ready.empty()) {
      int node = ready.top();
      ready.pop();
      result.push_back(node);

      for (int succ : successors[node]) {
        inDegree[succ]--;
        if (inDegree[succ] == 0) {
          ready.push(succ);
        }
      }
    }

    // If we didn't process all nodes, there's a cycle - add remaining nodes
    if (result.size() < n) {
      std::vector<bool> added(n, false);
      for (int idx : result) added[idx] = true;

      std::vector<int> remaining;
      for (size_t i = 0; i < n; ++i) {
        if (!added[i]) remaining.push_back(i);
      }
      // Sort remaining by tieBreaker
      std::sort(remaining.begin(), remaining.end(),
                [&](int a, int b) { return tieBreaker[a] < tieBreaker[b]; });
      for (int idx : remaining) result.push_back(idx);
    }

    return result;
  }

  // Ensemble sorting: combines 4 sorting methods into consensus order
  // 1. DFS sort (respects before/after via depth-first traversal)
  // 2. Kahn's algorithm (topological sort with plugin order tie-breaking)
  // 3. Plugin order (LOOT-sorted plugin positions)
  // 4. Collection order (original collection order)
  //
  // Each method votes for mod positions, then we combine votes while
  // respecting hard constraints (before/after rules) as bounds
  static std::vector<std::string>
  generateModOrderCombined(const std::vector<ModInfo> &mods,
                           const std::vector<ModRule> &rules,
                           const std::vector<std::string> &sortedPlugins,
                           const std::string &modsDir) {
    size_t n = mods.size();
    if (n == 0) return {};

    // Build lookup maps
    std::map<std::string, int> logicalNameToIdx;
    std::map<std::string, std::string> md5ToLogicalName;
    std::vector<std::string> modFolders;

    for (size_t i = 0; i < n; ++i) {
      std::string key = mods[i].logicalFilename.empty() ? mods[i].name : mods[i].logicalFilename;
      logicalNameToIdx[key] = i;
      std::string folder = mods[i].folderName.empty() ? mods[i].name : mods[i].folderName;
      modFolders.push_back(folder);
      if (!mods[i].md5.empty()) {
        md5ToLogicalName[mods[i].md5] = key;
      }
    }

    // Build plugin position map
    std::map<std::string, int> pluginPosition = buildPluginPositionMap(sortedPlugins);

    // Pre-compute plugin positions for each mod
    std::vector<int> modPluginPos(n);
    int modsWithPlugins = 0;
    for (size_t i = 0; i < n; ++i) {
      modPluginPos[i] = getModPluginPosition(modFolders[i], modsDir, pluginPosition);
      if (modPluginPos[i] < INT_MAX) modsWithPlugins++;
    }

    // Build adjacency lists for constraints
    std::vector<std::vector<int>> successors(n);   // successors[i] = mods that must come after i
    std::vector<std::vector<int>> predecessors(n); // predecessors[i] = mods that must come before i

    int appliedRules = 0;
    for (const auto &rule : rules) {
      std::string srcKey = rule.sourceLogicalName;
      if (srcKey.empty() && !rule.sourceMd5.empty()) {
        auto it = md5ToLogicalName.find(rule.sourceMd5);
        if (it != md5ToLogicalName.end()) srcKey = it->second;
      }

      std::string refKey = rule.referenceLogicalName;
      if (refKey.empty() && !rule.referenceMd5.empty()) {
        auto it = md5ToLogicalName.find(rule.referenceMd5);
        if (it != md5ToLogicalName.end()) refKey = it->second;
      }

      if (logicalNameToIdx.find(srcKey) == logicalNameToIdx.end() ||
          logicalNameToIdx.find(refKey) == logicalNameToIdx.end()) {
        continue;
      }

      int srcIdx = logicalNameToIdx[srcKey];
      int refIdx = logicalNameToIdx[refKey];

      if (rule.type == "before") {
        // source must come before reference
        successors[srcIdx].push_back(refIdx);
        predecessors[refIdx].push_back(srcIdx);
        appliedRules++;
      } else if (rule.type == "after") {
        // source must come after reference
        successors[refIdx].push_back(srcIdx);
        predecessors[srcIdx].push_back(refIdx);
        appliedRules++;
      }
    }

    std::cout << "  Applied " << appliedRules << " mod rules for sorting" << std::endl;
    std::cout << "  " << modsWithPlugins << "/" << n << " mods have plugins for position sorting" << std::endl;

    // =========================================================================
    // Method 1: DFS Sort (using existing generateModOrder logic)
    // =========================================================================
    std::vector<std::string> dfsOrder = generateModOrder(mods, rules);  // Returns folder names
    std::map<std::string, int> dfsPosition;
    for (size_t i = 0; i < dfsOrder.size(); ++i) {
      dfsPosition[dfsOrder[i]] = i;
    }
    std::vector<int> dfsRank(n);
    for (size_t i = 0; i < n; ++i) {
      auto it = dfsPosition.find(modFolders[i]);
      dfsRank[i] = (it != dfsPosition.end()) ? it->second : (int)i;
    }

    // =========================================================================
    // Method 2: Kahn's Algorithm (topological sort with plugin tie-breaking)
    // =========================================================================
    std::vector<int> kahnIndices = kahnSort(n, successors, predecessors, modPluginPos);
    std::vector<int> kahnRank(n);
    for (size_t i = 0; i < kahnIndices.size(); ++i) {
      kahnRank[kahnIndices[i]] = i;
    }

    // =========================================================================
    // Method 3: Plugin Order (sort purely by plugin position)
    // =========================================================================
    std::vector<int> pluginIndices(n);
    std::iota(pluginIndices.begin(), pluginIndices.end(), 0);
    std::stable_sort(pluginIndices.begin(), pluginIndices.end(),
                     [&](int a, int b) { return modPluginPos[a] < modPluginPos[b]; });
    std::vector<int> pluginRank(n);
    for (size_t i = 0; i < pluginIndices.size(); ++i) {
      pluginRank[pluginIndices[i]] = i;
    }

    // =========================================================================
    // Method 4: Collection Order (original order from collection)
    // =========================================================================
    std::vector<int> collectionRank(n);
    std::iota(collectionRank.begin(), collectionRank.end(), 0);

    // =========================================================================
    // Combine votes: weighted average of ranks
    // =========================================================================
    // Weights: DFS and Kahn respect constraints, so weight them higher
    // Plugin order is important for asset conflicts, collection order is baseline
    const double wDFS = 2.0;
    const double wKahn = 2.0;
    const double wPlugin = 1.5;
    const double wCollection = 0.5;
    const double totalWeight = wDFS + wKahn + wPlugin + wCollection;

    std::vector<double> combinedScore(n);
    for (size_t i = 0; i < n; ++i) {
      combinedScore[i] = (wDFS * dfsRank[i] +
                          wKahn * kahnRank[i] +
                          wPlugin * pluginRank[i] +
                          wCollection * collectionRank[i]) / totalWeight;
    }

    // =========================================================================
    // Final sort: Use Kahn's algorithm with combined score as tie-breaker
    // =========================================================================
    // Convert combined score to integer ranks for Kahn's tie-breaking
    std::vector<int> combinedRank(n);
    {
      std::vector<int> sortedByScore(n);
      std::iota(sortedByScore.begin(), sortedByScore.end(), 0);
      std::stable_sort(sortedByScore.begin(), sortedByScore.end(),
                       [&](int a, int b) { return combinedScore[a] < combinedScore[b]; });
      for (size_t i = 0; i < sortedByScore.size(); ++i) {
        combinedRank[sortedByScore[i]] = i;
      }
    }

    // Run Kahn's with combined rank as tie-breaker (respects constraints, breaks cycles gracefully)
    std::vector<int> finalIndices = kahnSort(n, successors, predecessors, combinedRank);

    // Count remaining violations (only direct constraints)
    std::map<int, int> finalPosition;
    for (size_t i = 0; i < finalIndices.size(); ++i) {
      finalPosition[finalIndices[i]] = i;
    }

    int violations = 0;
    for (size_t i = 0; i < n; ++i) {
      for (int pred : predecessors[i]) {
        if (finalPosition[pred] > finalPosition[i]) {
          violations++;
        }
      }
    }

    if (violations > 0) {
      std::cerr << "  [WARN] " << violations << " constraint violations (cycles in mod rules)" << std::endl;
    }

    std::cout << "  Ensemble sorting complete (DFS + Kahn + Plugin + Collection)" << std::endl;

    // Build result - MO2: Top = Winner, so reverse the order
    std::vector<std::string> result;
    result.reserve(n);
    for (auto it = finalIndices.rbegin(); it != finalIndices.rend(); ++it) {
      result.push_back(modFolders[*it]);
    }

    return result;
  }
};

// ============================================================================
// Plugin List Generator (plugins.txt) - Using LOOT for sorting
// ============================================================================

class PluginListGenerator {
public:
  // Find Skyrim SE local app data folder (for Steam/Proton on Linux)
  static std::string findLocalAppData() {
    // Steam Proton prefix for Skyrim SE (Steam App ID 489830)
    std::string home = getenv("HOME") ? getenv("HOME") : "";
    if (home.empty()) return "";

    // Check Proton prefix
    std::string protonPath = home + "/.local/share/Steam/steamapps/compatdata/489830/pfx/drive_c/users/steamuser/AppData/Local/Skyrim Special Edition";
    if (fs::exists(protonPath)) {
      return protonPath;
    }

    // Check regular Steam path
    std::string steamPath = home + "/.local/share/Steam/steamapps/compatdata/489830/pfx/drive_c/users/steamuser/AppData/Local";
    if (fs::exists(steamPath)) {
      return steamPath;
    }

    return "";
  }

  // Sort plugins using libloot
  static std::vector<std::string>
  sortPluginsWithLoot(const std::string &gamePath,
                      const std::string &modsDir,
                      const std::vector<PluginInfo> &plugins) {
    std::vector<std::string> sortedPlugins;

    try {
      // Find local app data folder
      std::string localPath = findLocalAppData();
      std::cout << "  Local app data: " << (localPath.empty() ? "(not found)" : localPath) << std::endl;

      // Create game handle for Skyrim SE
      auto game = loot::CreateGameHandle(
          loot::GameType::tes5se,
          fs::path(gamePath),
          localPath.empty() ? fs::path() : fs::path(localPath));

      // Set additional data paths (MO2 virtual filesystem mods)
      // Each mod folder's root is effectively a data path
      std::vector<fs::path> additionalPaths;
      if (fs::exists(modsDir)) {
        for (const auto &entry : fs::directory_iterator(modsDir)) {
          if (entry.is_directory()) {
            additionalPaths.push_back(entry.path());
          }
        }
      }
      if (!additionalPaths.empty()) {
        game->SetAdditionalDataPaths(additionalPaths);
      }

      // Collect unique plugin names that are enabled
      std::vector<std::string> pluginNames;
      std::set<std::string> seenPlugins;
      for (const auto &plugin : plugins) {
        if (plugin.enabled) {
          std::string nameLower = plugin.name;
          std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (seenPlugins.insert(nameLower).second) {
            pluginNames.push_back(plugin.name);
          }
        }
      }
      std::cout << "  Unique plugins: " << pluginNames.size() << " (from " << plugins.size() << " total)" << std::endl;

      // Find plugin files - track which plugins we've already found to avoid duplicates
      std::vector<fs::path> pluginPaths;
      std::set<std::string> foundPlugins; // Track by lowercase name

      for (const auto &pluginName : pluginNames) {
        std::string nameLower = pluginName;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Skip if we already found this plugin
        if (foundPlugins.count(nameLower)) continue;

        // Search for plugin in mods directory (first match wins - like MO2 priority)
        bool found = false;
        for (const auto &modDir : fs::directory_iterator(modsDir)) {
          if (!modDir.is_directory()) continue;
          fs::path pluginPath = modDir.path() / pluginName;
          if (fs::exists(pluginPath)) {
            pluginPaths.push_back(pluginPath);
            foundPlugins.insert(nameLower);
            found = true;
            break;
          }
        }
        if (!found) {
          // Try game Data folder
          fs::path gamePluginPath = fs::path(gamePath) / "Data" / pluginName;
          if (fs::exists(gamePluginPath)) {
            pluginPaths.push_back(gamePluginPath);
            foundPlugins.insert(nameLower);
          }
        }
      }

      std::cout << "  Loading " << pluginPaths.size() << " plugins for LOOT sorting..." << std::endl;

      // Load plugins (headers only for faster processing)
      game->LoadPlugins(pluginPaths, true);

      // Build list of plugins that actually exist (only sort what we loaded)
      std::vector<std::string> existingPluginNames;
      for (const auto &pluginName : pluginNames) {
        std::string nameLower = pluginName;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (foundPlugins.count(nameLower)) {
          existingPluginNames.push_back(pluginName);
        }
      }

      // Sort only plugins that exist
      sortedPlugins = game->SortPlugins(existingPluginNames);

      std::cout << "  LOOT sorted " << sortedPlugins.size() << " plugins" << std::endl;

    } catch (const std::exception &e) {
      std::cerr << "  [WARN] LOOT sorting failed: " << e.what() << std::endl;
      std::cerr << "  Falling back to collection order" << std::endl;

      // Fallback: use original order
      for (const auto &plugin : plugins) {
        if (plugin.enabled) {
          sortedPlugins.push_back(plugin.name);
        }
      }
    }

    return sortedPlugins;
  }

  static void writePluginList(const std::string &path,
                              const std::vector<std::string> &pluginOrder) {
    std::ofstream out(path);
    out << "# This file was automatically generated by NexusBridge"
        << std::endl;

    for (const auto &pluginName : pluginOrder) {
      out << "*" << pluginName << std::endl;
    }

    out.close();
    std::cout << "Generated plugins.txt with " << pluginOrder.size() << " plugins"
              << std::endl;
  }
};

// ============================================================================
// Collection URL Parser
// ============================================================================

struct CollectionUrlInfo {
  std::string game;
  std::string slug;
  bool valid;
};

CollectionUrlInfo parseCollectionUrl(const std::string &input) {
  CollectionUrlInfo info = {"", "", false};

  if (input.find("nexusmods.com") == std::string::npos &&
      input.find("http") == std::string::npos) {
    return info;
  }

  std::regex urlRe(
      R"(nexusmods\.com/(?:games/)?([^/]+)/collections/([^/?#]+))");
  std::smatch match;

  if (std::regex_search(input, match, urlRe)) {
    info.game = match[1].str();
    info.slug = match[2].str();
    info.valid = true;
  }

  return info;
}

// HTTP POST helper for GraphQL
std::string httpPost(const std::string &url, const std::string &body,
                     const std::string &apiKey) {
  CURL *curl = curl_easy_init();
  if (!curl) return "";

  std::string response;
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  std::string authHeader = "apikey: " + apiKey;
  headers = curl_slist_append(headers, authHeader.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) return "";
  return response;
}

std::string fetchCollectionFromNexus(const std::string &game,
                                     const std::string &slug,
                                     const std::string &apiKey) {
  if (apiKey.empty()) {
    std::cerr << "Error: Nexus API key required" << std::endl;
    return "";
  }

  std::cout << "Fetching collection from Nexus API..." << std::endl;
  std::cout << "  Game: " << game << std::endl;
  std::cout << "  Slug: " << slug << std::endl;

  // Use GraphQL API to get collection revision with download link
  std::string graphqlUrl = "https://api.nexusmods.com/v2/graphql";

  // GraphQL query to get the latest revision's download link
  std::string query = R"({
    "query": "query GetCollection($slug: String!) { collectionRevision(slug: $slug) { id revisionNumber downloadLink collection { name game { domainName } } modFiles { file { modId fileId name version uri game { domainName } } optional } } }",
    "variables": { "slug": ")" + slug + R"(" }
  })";

  std::string response = httpPost(graphqlUrl, query, apiKey);

  if (response.empty()) {
    std::cerr << "Failed to fetch collection from GraphQL API" << std::endl;
    return "";
  }

  try {
    json graphqlResponse = json::parse(response);

    if (graphqlResponse.contains("errors")) {
      std::cerr << "GraphQL Error: " << graphqlResponse["errors"].dump() << std::endl;
      return "";
    }

    if (!graphqlResponse.contains("data") ||
        !graphqlResponse["data"].contains("collectionRevision")) {
      std::cerr << "No collection data in response" << std::endl;
      return "";
    }

    auto revision = graphqlResponse["data"]["collectionRevision"];
    if (revision.is_null()) {
      std::cerr << "Collection revision is null (may be adult content blocked)" << std::endl;
      std::cerr << "  Response: " << graphqlResponse.dump(2) << std::endl;
      return "";
    }

    std::string downloadLink = revision.value("downloadLink", "");
    std::string collectionName = "Unknown";
    if (revision.contains("collection") && !revision["collection"].is_null()) {
      collectionName = revision["collection"].value("name", slug);
    }

    std::cout << "  Collection: " << collectionName << std::endl;
    std::cout << "  Revision: " << revision.value("revisionNumber", 0) << std::endl;
    std::cout << "  Download link: " << (downloadLink.empty() ? "(empty)" : downloadLink.substr(0, 100) + "...") << std::endl;

    if (downloadLink.empty()) {
      std::cerr << "No download link available (may require premium or adult content setting)" << std::endl;
      return "";
    }

    // The downloadLink endpoint returns a JSON with CDN links to a .7z archive
    // We need to: 1) Get the download links, 2) Download the .7z, 3) Extract collection.json
    std::string fullDownloadUrl = downloadLink;
    if (downloadLink[0] == '/') {
      fullDownloadUrl = "https://api.nexusmods.com" + downloadLink;
    }

    std::cout << "  Getting CDN download link..." << std::endl;
    std::string downloadLinksJson = httpGet(fullDownloadUrl, apiKey);

    if (downloadLinksJson.empty()) {
      std::cerr << "Failed to get download links" << std::endl;
      return "";
    }

    // Parse download links JSON to get CDN URL
    json linksResponse;
    try {
      linksResponse = json::parse(downloadLinksJson);
    } catch (const std::exception &e) {
      std::cerr << "Failed to parse download links: " << e.what() << std::endl;
      return "";
    }

    if (!linksResponse.contains("download_links") ||
        linksResponse["download_links"].empty()) {
      std::cerr << "No download links in response" << std::endl;
      return "";
    }

    std::string cdnUrl = linksResponse["download_links"][0].value("URI", "");
    if (cdnUrl.empty()) {
      std::cerr << "No CDN URI in download links" << std::endl;
      return "";
    }

    std::cout << "  Downloading collection archive..." << std::endl;

    // Download the .7z archive to a temp file
    fs::path archivePath = getTempDir() / ("nexusbridge_collection_" + slug + ".7z");

    CURL *curl = curl_easy_init();
    if (!curl) {
      std::cerr << "Failed to init curl" << std::endl;
      return "";
    }

    FILE *archiveFile = fopen(archivePath.string().c_str(), "wb");
    if (!archiveFile) {
      std::cerr << "Failed to create archive file" << std::endl;
      curl_easy_cleanup(curl);
      return "";
    }

    curl_easy_setopt(curl, CURLOPT_URL, cdnUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, archiveFile);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    CURLcode res = curl_easy_perform(curl);
    fclose(archiveFile);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
      std::cerr << "Failed to download archive: " << curl_easy_strerror(res) << std::endl;
      std::remove(archivePath.string().c_str());
      return "";
    }

    std::cout << "  Extracting collection.json..." << std::endl;

    // Extract collection.json from the .7z archive using 7za
    fs::path extractDir = getTempDir() / ("nexusbridge_collection_" + slug);
    fs::create_directories(extractDir);

#ifdef _WIN32
    std::string extractCmd = "\"" + get7zCommand() + "\" x -o\"" + extractDir.string() + "\" \"" + archivePath.string() + "\" collection.json -y > NUL 2>&1";
#else
    std::string extractCmd = get7zCommand() + " x -o" + extractDir.string() + " " + archivePath.string() + " collection.json -y > /dev/null 2>&1";
#endif
    int extractResult = std::system(extractCmd.c_str());

    if (extractResult != 0) {
      std::cerr << "Failed to extract collection.json from archive" << std::endl;
      std::remove(archivePath.string().c_str());
      return "";
    }

    fs::path collectionJsonPath = extractDir / "collection.json";
    if (!std::filesystem::exists(collectionJsonPath)) {
      std::cerr << "collection.json not found in archive" << std::endl;
      std::remove(archivePath.string().c_str());
      return "";
    }

    // Clean up archive
    std::remove(archivePath.string().c_str());

    std::cout << "  Extracted to: " << collectionJsonPath.string() << std::endl;
    return collectionJsonPath.string();

  } catch (const std::exception &e) {
    std::cerr << "Error parsing GraphQL response: " << e.what() << std::endl;
    return "";
  }
}

// ============================================================================
// Sanitize folder name for filesystem
// ============================================================================

std::string sanitizeFolderName(const std::string &name) {
  std::string result;
  for (char c : name) {
    // Replace problematic characters
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|') {
      result += '_';
    } else {
      result += c;
    }
  }
  // Trim trailing spaces/dots (Windows compatibility)
  while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
    result.pop_back();
  }
  return result;
}

// ============================================================================
// Main Application
// ============================================================================

void printUsage(const char *progName) {
  std::cout << "NexusBridge - Nexus Collections to MO2 Bridge (Independent)"
            << std::endl;
  std::cout << std::endl;
  std::cout << "Downloads mods directly from Nexus - NO Vortex required!"
            << std::endl;
  std::cout << std::endl;
  std::cout << "Usage:" << std::endl;
  std::cout << "  " << progName << " <collection_url> <mo2_path> [options]" << std::endl;
  std::cout << "  " << progName << " <collection.json> <mo2_path> [options]" << std::endl;
  std::cout << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  -y, --yes    Continue automatically on download failures" << std::endl;
  std::cout << std::endl;
  std::cout << "Arguments:" << std::endl;
  std::cout << "  collection_url    Nexus collection URL" << std::endl;
  std::cout << "  collection.json   Or path to local collection JSON file"
            << std::endl;
  std::cout << "  mo2_path          Path to MO2 instance directory"
            << std::endl;
  std::cout << std::endl;
  std::cout << "Requirements:" << std::endl;
  std::cout << "  - Nexus Premium membership (for direct downloads)"
            << std::endl;
  std::cout << "  - API key in: nexus_apikey.txt" << std::endl;
  std::cout << "  - 7z installed for archive extraction" << std::endl;
  std::cout << std::endl;
  std::cout << "Get your API key from: "
               "https://www.nexusmods.com/users/myaccount?tab=api"
            << std::endl;
}


int main(int argc, char *argv[]) {
  if (argc < 3) {
    printUsage(argv[0]);
    return 1;
  }

  std::string collectionInput = argv[1];
  std::string mo2Path = argv[2];

  // Parse optional flags
  bool autoYes = false;
  for (int i = 3; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-y" || arg == "--yes") {
      autoYes = true;
    }
  }

  // Setup paths
  std::string modsDir = mo2Path + "/mods";
  std::string downloadsDir = mo2Path + "/downloads";
  std::string profilesDir = mo2Path + "/profiles/Default";
  std::string tempDir = mo2Path + "/temp_extract";

  fs::create_directories(modsDir);
  fs::create_directories(downloadsDir);
  fs::create_directories(profilesDir);

  // Load API key
  std::string apiKey = loadApiKey("");
  if (apiKey.empty()) {
    std::cerr << "Error: Nexus API key required" << std::endl;
    std::cerr << "Create a file 'nexus_apikey.txt' with your API key"
              << std::endl;
    std::cerr << "Get your key from: "
                 "https://www.nexusmods.com/users/myaccount?tab=api"
              << std::endl;
    return 1;
  }

  // Load collection - either from URL or file
  std::string jsonContent;
  std::string gameDomain = "skyrimspecialedition"; // Default

  CollectionUrlInfo urlInfo = parseCollectionUrl(collectionInput);
  if (urlInfo.valid) {
    std::cout << "Detected Nexus collection URL" << std::endl;
    gameDomain = urlInfo.game;

    std::string collectionPath = fetchCollectionFromNexus(urlInfo.game, urlInfo.slug, apiKey);
    if (collectionPath.empty()) {
      return 1;
    }

    // Read the downloaded collection.json
    jsonContent = readFile(collectionPath);
    if (jsonContent.empty()) {
      std::cerr << "Failed to read downloaded collection" << std::endl;
      return 1;
    }

    // Copy to MO2 directory for reference
    std::string savedPath = mo2Path + "/collection_" + urlInfo.slug + ".json";
    std::ofstream out(savedPath);
    if (out) {
      out << jsonContent;
      out.close();
      std::cout << "Saved collection to: " << savedPath << std::endl;
    }
  } else {
    std::cout << "Loading collection: " << collectionInput << std::endl;
    jsonContent = readFile(collectionInput);
    if (jsonContent.empty()) {
      std::cerr << "Failed to read collection file" << std::endl;
      return 1;
    }
  }

  // Parse collection
  CollectionParser collection;
  if (!collection.parse(jsonContent)) {
    std::cerr << "Failed to parse collection" << std::endl;
    return 1;
  }

  gameDomain = collection.domainName;

  // Initialize Nexus API
  NexusAPI nexus(apiKey, gameDomain);
  if (!nexus.validateKey()) {
    return 1;
  }

  if (!nexus.isPremium) {
    std::cerr << "ERROR: Nexus Premium is required for direct downloads."
              << std::endl;
    std::cerr << "Without Premium, the API does not provide download links."
              << std::endl;
    return 1;
  }

  // Process each mod - THREADED
  std::cout << std::endl
            << "Processing " << collection.mods.size() << " mods..."
            << std::endl;

  // Determine thread count (use hardware concurrency, min 4)
  unsigned int numThreads = std::thread::hardware_concurrency();
  if (numThreads == 0)
    numThreads = 8;  // Fallback if hardware_concurrency() returns 0
  else if (numThreads < 4)
    numThreads = 4;
  std::cout << "Using " << numThreads << " threads for parallel operations"
            << std::endl;

  int downloaded = 0;
  int skipped = 0;

  // Reset global counters
  g_installed = 0;
  g_failed = 0;

  // Collect install tasks
  std::vector<InstallTask> installTasks;

  std::cout << std::endl << "=== Phase 1: Scanning archives ===" << std::endl;

  // Store archive paths for each mod index
  std::map<size_t, std::string> modArchivePaths;
  std::map<size_t, std::string> modFolderNames;
  std::vector<DownloadTask> downloadTasks;

  // First pass: identify which mods need downloading
  for (size_t i = 0; i < collection.mods.size(); ++i) {
    auto &mod = collection.mods[i];

    bool isDirectDownload =
        (mod.sourceType == "direct" && !mod.directUrl.empty());

    if (!isDirectDownload && (mod.modId <= 0 || mod.fileId <= 0)) {
      skipped++;
      continue;
    }

    // Create folder name
    std::string folderName;
    if (isDirectDownload) {
      folderName = sanitizeFolderName(mod.name);
    } else {
      folderName = sanitizeFolderName(
          mod.logicalFilename.empty() ? mod.name : mod.logicalFilename);
      folderName +=
          "-" + std::to_string(mod.modId) + "-" + std::to_string(mod.fileId);
    }

    std::string destModPath = modsDir + "/" + folderName;
    mod.folderName = folderName;
    modFolderNames[i] = folderName;

    // Skip if already installed
    if (fs::exists(destModPath) && !fs::is_empty(destModPath)) {
      skipped++;
      continue;
    }

    std::string filename;
    std::string archivePath;

    if (isDirectDownload) {
      size_t lastSlash = mod.directUrl.rfind('/');
      filename = (lastSlash != std::string::npos)
                     ? mod.directUrl.substr(lastSlash + 1)
                     : mod.name + ".7z";
      archivePath = downloadsDir + "/" + filename;

      if (!fs::exists(archivePath) || fs::file_size(archivePath) == 0) {
        DownloadTask dt;
        dt.url = mod.directUrl;
        dt.destPath = archivePath;
        dt.filename = filename;
        dt.modName = mod.name;
        dt.fileSize = mod.fileSize;
        dt.modId = mod.modId;
        dt.fileId = mod.fileId;
        dt.isDirectDownload = true;
        dt.modIndex = i;
        downloadTasks.push_back(dt);
      } else {
        modArchivePaths[i] = archivePath;
      }
    } else {
      // Nexus - try to find existing archive
      std::string modIdPattern = "-" + std::to_string(mod.modId) + "-";
      std::string logicalLower = mod.logicalFilename;
      std::transform(logicalLower.begin(), logicalLower.end(), logicalLower.begin(),
                     [](unsigned char c) { return std::tolower(c); });

      bool found = false;
      std::string bestMatch;
      std::string fallbackMatch;
      long long expectedSize = mod.fileSize;

      for (const auto &entry : fs::directory_iterator(downloadsDir)) {
        std::string fname = entry.path().filename().string();
        std::string fnameLower = fname;
        std::transform(fnameLower.begin(), fnameLower.end(), fnameLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (!logicalLower.empty()) {
          std::string expectedStart = logicalLower + modIdPattern;
          std::transform(expectedStart.begin(), expectedStart.end(), expectedStart.begin(),
                         [](unsigned char c) { return std::tolower(c); });

          if (fnameLower.find(expectedStart) == 0) {
            archivePath = entry.path().string();
            modArchivePaths[i] = archivePath;
            found = true;
            break;
          }

          std::string ccPrefix = "creation club - ";
          size_t ccPos = logicalLower.find(ccPrefix);
          if (ccPos != std::string::npos) {
            std::string simplifiedLogical = logicalLower.substr(0, ccPos) + logicalLower.substr(ccPos + ccPrefix.length());
            std::string simplifiedStart = simplifiedLogical + modIdPattern;
            if (fnameLower.find(simplifiedStart) == 0) {
              archivePath = entry.path().string();
              modArchivePaths[i] = archivePath;
              found = true;
              break;
            }
          }
        }

        if (!found && fname.find(modIdPattern) != std::string::npos) {
          long long actualSize = static_cast<long long>(fs::file_size(entry.path()));
          if (expectedSize > 0 && actualSize == expectedSize) {
            archivePath = entry.path().string();
            modArchivePaths[i] = archivePath;
            found = true;
            break;
          } else if (fallbackMatch.empty()) {
            fallbackMatch = entry.path().string();
          }
        }
      }

      if (!found && !fallbackMatch.empty()) {
        archivePath = fallbackMatch;
        modArchivePaths[i] = archivePath;
        found = true;
      }

      if (!found) {
        DownloadTask dt;
        dt.modName = mod.name;
        dt.fileSize = mod.fileSize;
        dt.modId = mod.modId;
        dt.fileId = mod.fileId;
        dt.isDirectDownload = false;
        dt.modIndex = i;
        downloadTasks.push_back(dt);
      }
    }
  }

  std::cout << "  Found " << modArchivePaths.size() << " existing archives" << std::endl;
  std::cout << "  Need to download " << downloadTasks.size() << " archives" << std::endl;

  // Phase 1b: Download missing archives in parallel
  if (!downloadTasks.empty()) {
    std::cout << std::endl << "=== Phase 1b: Downloading " << downloadTasks.size()
              << " archives with " << numThreads << " threads ===" << std::endl;

    std::atomic<size_t> downloadIndex{0};
    std::atomic<int> downloadedCount{0};
    std::mutex downloadMutex;
    std::vector<size_t> failedIndices;  // Track failed download indices

    // Download worker function - processes tasks from downloadTasks
    auto downloadWorker = [&](const std::vector<DownloadTask>& tasks,
                              std::atomic<size_t>& taskIdx,
                              std::vector<size_t>& failed,
                              bool isRetry) {
      while (true) {
        size_t idx = taskIdx.fetch_add(1);
        if (idx >= tasks.size()) break;

        const auto& dt = tasks[idx];
        std::string archivePath;

        {
          std::lock_guard<std::mutex> lock(downloadMutex);
          if (isRetry) {
            std::cout << "  [Retry] Downloading: " << dt.modName << std::endl;
          } else {
            std::cout << "  [" << (idx + 1) << "/" << tasks.size()
                      << "] Downloading: " << dt.modName << std::endl;
          }
        }

        bool success = false;
        if (dt.isDirectDownload) {
          archivePath = dt.destPath;
          success = downloadFile(dt.url, archivePath, dt.filename, dt.fileSize);
        } else {
          auto links = nexus.getDownloadLinks(dt.modId, dt.fileId);
          if (!links.empty()) {
            std::string downloadUrl = links[0];  // Already a string URL
            std::string filename = dt.modName + "-" + std::to_string(dt.modId) +
                                   "-" + std::to_string(dt.fileId) + ".7z";
            for (char& c : filename) {
              if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                  c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
              }
            }
            archivePath = downloadsDir + "/" + filename;
            success = downloadFile(downloadUrl, archivePath, filename, dt.fileSize);
          }
        }

        if (success && !archivePath.empty()) {
          std::lock_guard<std::mutex> lock(downloadMutex);
          modArchivePaths[dt.modIndex] = archivePath;
          downloadedCount++;
          downloaded++;
        } else {
          std::lock_guard<std::mutex> lock(downloadMutex);
          failed.push_back(idx);
          std::cout << "  FAILED: No download links for " << dt.modName << std::endl;
        }
      }
    };

    // Initial download pass
    std::vector<std::thread> downloadThreads;
    for (unsigned int t = 0; t < numThreads; ++t) {
      downloadThreads.emplace_back([&]() {
        downloadWorker(downloadTasks, downloadIndex, failedIndices, false);
      });
    }
    for (auto& t : downloadThreads) {
      t.join();
    }

    // Retry failed downloads up to 3 times
    const int maxRetries = 3;
    for (int retry = 1; retry <= maxRetries && !failedIndices.empty(); ++retry) {
      std::cout << std::endl << "  === Retry " << retry << "/" << maxRetries
                << " for " << failedIndices.size() << " failed downloads ===" << std::endl;

      // Small delay before retry to let server recover
      std::this_thread::sleep_for(std::chrono::seconds(2));

      // Build retry task list
      std::vector<DownloadTask> retryTasks;
      for (size_t idx : failedIndices) {
        retryTasks.push_back(downloadTasks[idx]);
      }

      failedIndices.clear();
      std::atomic<size_t> retryIndex{0};

      // Retry with fewer threads to be gentler on the API
      unsigned int retryThreads = std::min(numThreads, 4u);
      std::vector<std::thread> retryThreadPool;
      for (unsigned int t = 0; t < retryThreads; ++t) {
        retryThreadPool.emplace_back([&]() {
          downloadWorker(retryTasks, retryIndex, failedIndices, true);
        });
      }
      for (auto& t : retryThreadPool) {
        t.join();
      }
    }

    int failedDownloads = static_cast<int>(failedIndices.size());
    std::cout << "  Downloaded: " << downloadedCount << ", Failed: " << failedDownloads << std::endl;

    // If there are still failures after retries, ask user if they want to continue
    if (failedDownloads > 0) {
      std::cout << std::endl;
      std::cout << "WARNING: " << failedDownloads << " mod(s) failed to download after "
                << maxRetries << " retries:" << std::endl;
      for (size_t idx : failedIndices) {
        std::cout << "  - " << downloadTasks[idx].modName << std::endl;
      }
      std::cout << std::endl;

      if (autoYes) {
        std::cout << "Auto-continuing due to --yes flag..." << std::endl;
      } else {
        std::cout << "Continue anyway? This may cause issues with your mod setup. [y/N]: ";
        std::cout.flush();

        std::string response;
        std::getline(std::cin, response);

        if (response.empty() || (response[0] != 'y' && response[0] != 'Y')) {
          std::cout << "Installation cancelled by user." << std::endl;
          return 1;
        }
      }
      std::cout << "Continuing with installation..." << std::endl;
    }
  }

  // Phase 2: Install mods in parallel
  for (const auto& [idx, archivePath] : modArchivePaths) {
    InstallTask task;
    task.archivePath = archivePath;
    task.destModPath = modsDir + "/" + modFolderNames[idx];
    task.tempDir = tempDir + "/" + modFolderNames[idx];
    task.folderName = modFolderNames[idx];
    task.modName = collection.mods[idx].name;
    task.choices = collection.mods[idx].choices;
    task.index = idx;
    task.total = collection.mods.size();
    task.expectedPaths = collection.mods[idx].expectedPaths;
    installTasks.push_back(task);
  }

  if (!installTasks.empty()) {
    std::cout << std::endl << "=== Phase 2: Installing " << installTasks.size()
              << " mods with " << numThreads << " threads ===" << std::endl;

    std::atomic<size_t> taskIndex{0};

    auto installWorker = [&]() {
      while (true) {
        size_t idx = taskIndex.fetch_add(1);
        if (idx >= installTasks.size()) break;

        const auto& task = installTasks[idx];
        installMod(task);
      }
    };

    std::vector<std::thread> installThreads;
    for (unsigned int t = 0; t < numThreads; ++t) {
      installThreads.emplace_back(installWorker);
    }
    for (auto& t : installThreads) {
      t.join();
    }
  }

  int installed = g_installed.load();
  int failed = g_failed.load();

  // Generate plugins.txt with LOOT sorting
  std::cout << std::endl << "Generating plugins.txt..." << std::endl;

  std::string gamePath = mo2Path + "/Stock Game";
  if (!fs::exists(gamePath)) {
    std::ifstream iniFile(mo2Path + "/ModOrganizer.ini");
    if (iniFile) {
      std::string line;
      while (std::getline(iniFile, line)) {
        if (line.find("gamePath=") != std::string::npos) {
          size_t pos = line.find("=");
          if (pos != std::string::npos) {
            gamePath = line.substr(pos + 1);
            while (!gamePath.empty() && (gamePath.back() == '\r' || gamePath.back() == '\n')) {
              gamePath.pop_back();
            }
            if (!gamePath.empty() && gamePath.front() == '@') {
              gamePath = gamePath.substr(1);
              size_t atPos = gamePath.find("@");
              if (atPos != std::string::npos) {
                gamePath = mo2Path + "/" + gamePath.substr(atPos + 1);
              }
            }
            break;
          }
        }
      }
    }
  }

  if (gamePath.empty()) {
    std::string steamPath = std::string(getenv("HOME") ? getenv("HOME") : "") +
                            "/.local/share/Steam/steamapps/common/Skyrim Special Edition";
    if (fs::exists(steamPath)) {
      gamePath = steamPath;
    }
  }

  std::vector<std::string> pluginOrder;
  if (!gamePath.empty() && fs::exists(gamePath)) {
    std::cout << "  Using game path: " << gamePath << std::endl;
    pluginOrder = PluginListGenerator::sortPluginsWithLoot(gamePath, modsDir, collection.plugins);
  } else {
    std::cerr << "  [WARN] Could not find game path, using collection order" << std::endl;
    for (const auto &plugin : collection.plugins) {
      if (plugin.enabled) {
        pluginOrder.push_back(plugin.name);
      }
    }
  }

  PluginListGenerator::writePluginList(profilesDir + "/plugins.txt", pluginOrder);

  // Generate modlist.txt using combined sorting
  std::cout << "Generating modlist.txt..." << std::endl;
  std::vector<std::string> modOrder =
      ModListGenerator::generateModOrderCombined(collection.mods, collection.modRules, pluginOrder, modsDir);

  ModListGenerator::writeModList(profilesDir + "/modlist.txt", modOrder);

  // Summary
  std::cout << std::endl << "=== Summary ===" << std::endl;
  std::cout << "Downloaded: " << downloaded << std::endl;
  std::cout << "Installed:  " << installed << std::endl;
  std::cout << "Skipped:    " << skipped << " (already installed)" << std::endl;
  std::cout << "Failed:     " << failed << std::endl;
  std::cout << std::endl
            << "Done! Please restart Mod Organizer 2." << std::endl;

  return (failed > 0) ? 1 : 0;
}
