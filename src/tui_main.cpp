/**
 * NexusBridge TUI - Terminal User Interface
 *
 * A modern TUI for installing Nexus Collections to MO2
 */

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <limits.h>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #define PATH_MAX MAX_PATH
    #define popen _popen
    #define pclose _pclose
    #ifndef S_ISDIR
        #define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
    #endif
#else
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace ftxui;

// Get the directory containing this executable
std::string getExecutableDir() {
  static std::string dir;
  if (!dir.empty()) return dir;

  char buf[PATH_MAX];
#ifdef _WIN32
  DWORD len = GetModuleFileNameA(NULL, buf, PATH_MAX);
  if (len > 0) {
    fs::path p(buf);
    dir = p.parent_path().string();
    return dir;
  }
#else
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len != -1) {
    buf[len] = '\0';
    fs::path p(buf);
    dir = p.parent_path().string();
    return dir;
  }
#endif

  dir = fs::current_path().string();
  return dir;
}

// Find NexusBridge executable (same directory as TUI)
std::string findNexusBridge() {
  std::string exeDir = getExecutableDir();
  std::string sameDirPath = exeDir + "/NexusBridge";
  if (fs::exists(sameDirPath)) {
    return sameDirPath;
  }

  // Fallback: try current directory
  if (fs::exists("./NexusBridge")) {
    return "./NexusBridge";
  }

  return "";
}

// Installation phase
enum class InstallPhase {
  Starting,
  Scanning,
  Downloading,
  Installing,
  Generating,
  Complete,
  Error
};

std::string phaseToString(InstallPhase phase) {
  switch (phase) {
    case InstallPhase::Starting: return "Starting...";
    case InstallPhase::Scanning: return "Scanning archives...";
    case InstallPhase::Downloading: return "Downloading...";
    case InstallPhase::Installing: return "Installing mods...";
    case InstallPhase::Generating: return "Generating load order...";
    case InstallPhase::Complete: return "Complete!";
    case InstallPhase::Error: return "Error!";
  }
  return "";
}

// Application state
struct AppState {
  // Settings
  std::string apiKey;
  std::string mo2Path;

  // Installation progress
  std::atomic<bool> installing{false};
  std::atomic<int> totalMods{0};
  std::atomic<int> toDownload{0};
  std::atomic<int> downloading{0};  // Currently downloading (started but not counted as complete)
  std::atomic<int> downloaded{0};
  std::atomic<int> downloadFailed{0};
  std::atomic<int> toInstall{0};
  std::atomic<int> installed{0};
  std::atomic<int> skipped{0};
  std::atomic<int> failed{0};

  // Current phase
  std::atomic<InstallPhase> phase{InstallPhase::Starting};
  std::atomic<bool> hasError{false};

  // Log messages (filtered - no download spam)
  std::mutex logMutex;
  std::vector<std::string> logMessages;

  void addLog(const std::string& msg) {
    // Filter out download progress spam - but keep "[X/Y] Downloading: ModName" lines

    // Allow "[X/Y] Downloading: ModName" lines through (they show which mod is being downloaded)
    bool isModDownloadLine = (msg.find("] Downloading:") != std::string::npos &&
                               msg.find("[") != std::string::npos &&
                               msg.find(" MB (") == std::string::npos);

    if (!isModDownloadLine) {
      // Pattern 1: Full line "Downloading: X.X / X.X MB (XX%)"
      if (msg.find("Downloading:") != std::string::npos &&
          msg.find(" MB (") != std::string::npos) {
        return;
      }
      // Pattern 2: Partial line from carriage return - just percentage "(XX%)"
      if (msg.find("%)") != std::string::npos && msg.length() < 50) {
        return;
      }
      // Pattern 3: Partial line "nloading:" (from "Downloading:" after \r)
      if (msg.find("nloading:") != std::string::npos) {
        return;
      }
      // Pattern 4: Lines starting with whitespace then "Downloading:"
      std::string trimmed = msg;
      size_t start = trimmed.find_first_not_of(" \t\r");
      if (start != std::string::npos) {
        trimmed = trimmed.substr(start);
      }
      if (trimmed.find("Downloading:") == 0 && trimmed.find(" MB") != std::string::npos) {
        return;
      }
      // Pattern 5: Skip carriage return lines
      if (!msg.empty() && msg[0] == '\r') {
        return;
      }
      // Pattern 6: Skip lines that are just numbers and slashes (partial progress)
      if (msg.length() < 30 && msg.find(" / ") != std::string::npos) {
        return;
      }
    }

    std::lock_guard<std::mutex> lock(logMutex);
    logMessages.push_back(msg);
    if (logMessages.size() > 100) {
      logMessages.erase(logMessages.begin());
    }
  }

  std::vector<std::string> getLogs() {
    std::lock_guard<std::mutex> lock(logMutex);
    return logMessages;
  }

  // Get overall progress (0.0 to 1.0)
  float getOverallProgress() {
    InstallPhase currentPhase = phase.load();
    if (currentPhase == InstallPhase::Starting || currentPhase == InstallPhase::Scanning) {
      return 0.0f;
    }
    if (currentPhase == InstallPhase::Complete) {
      return 1.0f;
    }
    if (currentPhase == InstallPhase::Generating) {
      return 0.95f;  // Almost done
    }

    int dlTotal = toDownload.load();
    int instTotal = toInstall.load();
    if (instTotal == 0) instTotal = dlTotal;  // Fallback

    int totalWork = dlTotal + instTotal;
    if (totalWork == 0) return 0.0f;

    // During download phase, use downloading count
    int dlDone = (currentPhase == InstallPhase::Downloading)
                  ? downloading.load()
                  : downloaded.load();
    int instDone = installed.load();

    return static_cast<float>(dlDone + instDone) / totalWork;
  }
};

// Load settings from file
void loadSettings(AppState& state) {
  std::string configDir = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config/nexusbridge";
  fs::create_directories(configDir);

  // Load API key
  std::string keyFile = configDir + "/apikey.txt";
  if (fs::exists(keyFile)) {
    std::ifstream f(keyFile);
    std::getline(f, state.apiKey);
  }

  // Load MO2 path
  std::string pathFile = configDir + "/mo2path.txt";
  if (fs::exists(pathFile)) {
    std::ifstream f(pathFile);
    std::getline(f, state.mo2Path);
  }

  // Default MO2 path
  if (state.mo2Path.empty()) {
    state.mo2Path = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/Documents/MO2";
  }
}

// Save settings to file
void saveSettings(const AppState& state) {
  std::string configDir = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config/nexusbridge";
  fs::create_directories(configDir);

  std::ofstream keyFile(configDir + "/apikey.txt");
  keyFile << state.apiKey;

  std::ofstream pathFile(configDir + "/mo2path.txt");
  pathFile << state.mo2Path;
}

// Get tab completions for a path
std::vector<std::string> getPathCompletions(const std::string& partial) {
  if (partial.empty()) return {};

  fs::path path(partial);
  fs::path parent = path.parent_path();
  std::string prefix = path.filename().string();

  // If partial ends with /, list that directory
  if (partial.back() == '/') {
    parent = path;
    prefix = "";
  }

  std::vector<std::string> completions;

  try {
    if (fs::exists(parent) && fs::is_directory(parent)) {
      for (const auto& entry : fs::directory_iterator(parent)) {
        std::string name = entry.path().filename().string();
        if (name.find(prefix) == 0) {
          std::string full = entry.path().string();
          if (entry.is_directory()) {
            full += "/";
          }
          completions.push_back(full);
        }
      }
      std::sort(completions.begin(), completions.end());
    }
  } catch (...) {
    // Ignore errors
  }

  return completions;
}

// Main menu screen
enum class AppScreen {
  MainMenu,
  Install,
  Installing,
  Settings,
  About
};

int main() {
  AppState state;
  loadSettings(state);

  auto screen = ScreenInteractive::Fullscreen();
  AppScreen currentScreen = AppScreen::MainMenu;

  int mainMenuSelected = 0;

  // Input fields
  std::string collectionUrl;
  std::string apiKeyInput = state.apiKey;
  std::string mo2PathInput = state.mo2Path;

  // Tab completion state
  std::vector<std::string> completions;
  size_t completionIndex = 0;

  // Installation thread
  std::thread installThread;

  // Components for each screen
  auto mainMenuEntries = std::vector<std::string>{
    "Install Collection",
    "Settings",
    "About",
    "Exit"
  };

  auto mainMenu = Menu(&mainMenuEntries, &mainMenuSelected);

  // Collection URL input with styling
  InputOption collectionOpt;
  collectionOpt.multiline = false;
  auto collectionInput = Input(&collectionUrl, "Collection URL or path...", collectionOpt);

  // Settings inputs - use InputOption for better cursor visibility
  InputOption apiKeyOpt;
  apiKeyOpt.multiline = false;
  auto apiKeyInputComp = Input(&apiKeyInput, "Enter your Nexus API Key here...", apiKeyOpt);

  // MO2 path input with cursor position tracking for tab completion
  int mo2CursorPos = static_cast<int>(mo2PathInput.size());
  InputOption mo2PathOpt;
  mo2PathOpt.multiline = false;
  mo2PathOpt.cursor_position = &mo2CursorPos;
  auto mo2PathInputComp = Input(&mo2PathInput, "Enter MO2 directory path...", mo2PathOpt);

  // Settings container with vertical navigation - this handles focus properly
  auto settingsContainer = Container::Vertical({
    apiKeyInputComp,
    mo2PathInputComp,
  });

  // Main component that switches between screens
  auto mainComponent = Container::Tab({
    mainMenu,           // MainMenu
    collectionInput,    // Install
    Container::Vertical({}),  // Installing (no input)
    settingsContainer,  // Settings
    Container::Vertical({}),  // About (no input)
  }, reinterpret_cast<int*>(&currentScreen));

  // Wrap with renderer for custom UI
  auto renderer = Renderer(mainComponent, [&] {
    Elements content;

    // Header - centered
    auto headerText = "NexusBridge - Collection Installer";
    content.push_back(
      hbox({
        filler(),
        text(headerText) | bold | color(Color::Cyan) | border,
        filler(),
      })
    );
    content.push_back(separator());

    switch (currentScreen) {
      case AppScreen::MainMenu: {
        content.push_back(text(" Main Menu") | bold);
        content.push_back(separator());
        content.push_back(mainMenu->Render() | frame);
        content.push_back(filler());
        content.push_back(separator());
        content.push_back(text(" [Enter] Select  [Q] Quit") | dim);
        break;
      }

      case AppScreen::Install: {
        content.push_back(text(" Install Collection") | bold);
        content.push_back(separator());
        content.push_back(text(" Enter collection URL or path:"));
        content.push_back(hbox({
          text(" > ") | color(Color::Green),
          collectionInput->Render() | flex | border | bgcolor(Color::GrayDark),
        }));
        content.push_back(text(""));
        content.push_back(text(" Examples:") | dim);
        content.push_back(text("   https://www.nexusmods.com/skyrimspecialedition/collections/qdurkx") | dim);
        content.push_back(text("   /path/to/collection.json") | dim);
        content.push_back(filler());
        content.push_back(separator());
        content.push_back(text(" [Enter] Start Install  [Esc] Back") | dim);
        break;
      }

      case AppScreen::Installing: {
        content.push_back(text(" Installing Collection") | bold);
        content.push_back(separator());

        // Phase indicator
        InstallPhase currentPhase = state.phase.load();
        Color phaseColor = Color::Cyan;
        if (currentPhase == InstallPhase::Complete) phaseColor = Color::Green;
        if (currentPhase == InstallPhase::Error) phaseColor = Color::Red;
        content.push_back(hbox({
          text(" Phase: ") | bold,
          text(phaseToString(currentPhase)) | color(phaseColor),
        }));

        content.push_back(text(""));

        // Overall progress bar
        float progress = state.getOverallProgress();
        int progressPct = static_cast<int>(progress * 100);
        content.push_back(hbox({
          text(" Overall: "),
          gauge(progress) | flex | color(Color::Green),
          text(" " + std::to_string(progressPct) + "%"),
        }));

        content.push_back(text(""));

        // Stats line - show different info based on phase
        int toDownload = state.toDownload.load();
        int downloading = state.downloading.load();
        int downloaded = state.downloaded.load();
        int toInstall = state.toInstall.load();
        int installed = state.installed.load();
        int skipped = state.skipped.load();
        int failed = state.failed.load();
        int downloadFailed = state.downloadFailed.load();

        content.push_back(hbox({
          text(" Total: ") | bold,
          text(std::to_string(state.totalMods.load())) | color(Color::White),
          text("  Downloaded: ") | bold,
          // During downloading phase, show downloading count; after, show final count
          text(currentPhase == InstallPhase::Downloading
               ? std::to_string(downloading) + "/" + std::to_string(toDownload)
               : std::to_string(downloaded) + "/" + std::to_string(toDownload)) | color(Color::Cyan),
          text("  Installed: ") | bold,
          text(std::to_string(installed) + "/" +
               std::to_string(toInstall > 0 ? toInstall : toDownload)) | color(Color::Green),
          text("  Skipped: ") | bold,
          text(std::to_string(skipped)) | color(Color::Yellow),
          text("  Failed: ") | bold,
          text(std::to_string(failed + downloadFailed)) | color(Color::Red),
        }));

        content.push_back(separator());
        content.push_back(text(" Log:") | bold);

        auto logs = state.getLogs();
        Elements logElements;
        int startIdx = std::max(0, static_cast<int>(logs.size()) - 12);
        for (size_t i = startIdx; i < logs.size(); ++i) {
          // Color code log lines
          auto& line = logs[i];
          Element lineEl;
          if (line.find("ERROR") != std::string::npos ||
              line.find("Failed") != std::string::npos) {
            lineEl = text(" " + line) | color(Color::Red);
          } else if (line.find("Done!") != std::string::npos ||
                     line.find("Complete") != std::string::npos) {
            lineEl = text(" " + line) | color(Color::Green);
          } else if (line.find("Downloading:") != std::string::npos ||
                     line.find("Installing") != std::string::npos) {
            lineEl = text(" " + line) | color(Color::Cyan);
          } else if (line.find("[") != std::string::npos &&
                     line.find("/") != std::string::npos) {
            lineEl = text(" " + line) | color(Color::Yellow);
          } else {
            lineEl = text(" " + line) | dim;
          }
          logElements.push_back(lineEl);
        }
        content.push_back(vbox(logElements) | flex | frame);

        content.push_back(separator());
        if (state.installing) {
          content.push_back(text(" Installing... Please wait.") | color(Color::Yellow));
        } else {
          content.push_back(text(" Installation complete! [Enter] Back to Menu") | color(Color::Green));
        }
        break;
      }

      case AppScreen::Settings: {
        content.push_back(text(" Settings") | bold);
        content.push_back(separator());

        // Check which input is focused
        bool apiKeyFocused = apiKeyInputComp->Focused();
        bool mo2PathFocused = mo2PathInputComp->Focused();

        // API Key field
        auto apiLabel = apiKeyFocused ? " > Nexus API Key:" : "   Nexus API Key:";
        content.push_back(text(apiLabel) | (apiKeyFocused ? color(Color::Green) : nothing));
        content.push_back(hbox({
          text("   "),
          apiKeyInputComp->Render() | flex | border |
            (apiKeyFocused ? bgcolor(Color::GrayDark) : nothing),
        }));
        content.push_back(text("   (Get Personal API Key from: https://next.nexusmods.com/settings/api-keys)") | dim);
        content.push_back(text("   (Scroll to bottom of page)") | dim);

        content.push_back(text(""));

        // MO2 Directory field
        auto mo2Label = mo2PathFocused ? " > MO2 Directory:" : "   MO2 Directory:";
        content.push_back(text(mo2Label) | (mo2PathFocused ? color(Color::Green) : nothing));
        content.push_back(hbox({
          text("   "),
          mo2PathInputComp->Render() | flex | border |
            (mo2PathFocused ? bgcolor(Color::GrayDark) : nothing),
        }));

        // Show tab completions if available
        if (mo2PathFocused && !completions.empty()) {
          content.push_back(text("   Tab completions:") | dim);
          for (size_t i = 0; i < std::min(completions.size(), size_t(5)); ++i) {
            auto compText = "     " + completions[i];
            if (i == completionIndex) {
              content.push_back(text(compText) | color(Color::Yellow) | bold);
            } else {
              content.push_back(text(compText) | dim);
            }
          }
          if (completions.size() > 5) {
            content.push_back(text("     ... and " + std::to_string(completions.size() - 5) + " more") | dim);
          }
        }

        content.push_back(filler());
        content.push_back(separator());
        content.push_back(text(" [Tab] Complete path / Switch field  [Enter] Save  [Esc] Cancel") | dim);
        break;
      }

      case AppScreen::About: {
        content.push_back(text(" About NexusBridge") | bold);
        content.push_back(separator());
        content.push_back(text(""));
        content.push_back(text(" NexusBridge v2.0") | bold);
        content.push_back(text(" Install Nexus Collections directly to Mod Organizer 2"));
        content.push_back(text(""));
        content.push_back(text(" Features:") | bold);
        content.push_back(text("   - Direct CDN downloads (Premium required)"));
        content.push_back(text("   - Automatic FOMOD installation"));
        content.push_back(text("   - LOOT-based plugin sorting"));
        content.push_back(text("   - Mod rule enforcement"));
        content.push_back(text("   - Parallel downloads & installs"));
        content.push_back(text(""));
        content.push_back(text(" Created for Linux MO2 users") | dim);
        content.push_back(filler());
        content.push_back(separator());
        content.push_back(text(" [Esc] Back") | dim);
        break;
      }
    }

    return vbox(content) | border;
  });

  // Event handler
  auto component = CatchEvent(renderer, [&](Event event) {
    // Global quit
    if (event == Event::Character('q') || event == Event::Character('Q')) {
      if (currentScreen == AppScreen::MainMenu && !state.installing) {
        screen.ExitLoopClosure()();
        return true;
      }
    }

    // Escape to go back
    if (event == Event::Escape) {
      if (currentScreen != AppScreen::MainMenu && !state.installing) {
        currentScreen = AppScreen::MainMenu;
        completions.clear();
        return true;
      }
    }

    // Handle Tab for path completion in Settings
    if (event == Event::Tab && currentScreen == AppScreen::Settings) {
      if (mo2PathInputComp->Focused()) {
        // Path completion mode
        if (completions.empty()) {
          completions = getPathCompletions(mo2PathInput);
          completionIndex = 0;
        } else {
          completionIndex = (completionIndex + 1) % completions.size();
        }

        if (!completions.empty()) {
          mo2PathInput = completions[completionIndex];
          // Move cursor to end of input
          mo2CursorPos = static_cast<int>(mo2PathInput.size());
        }
        return true;
      }
      // Otherwise let Tab switch fields (default behavior)
      completions.clear();
    }

    // Clear completions on any other key in Settings
    if (currentScreen == AppScreen::Settings && event.is_character()) {
      completions.clear();
    }

    // Handle enter based on screen
    if (event == Event::Return) {
      switch (currentScreen) {
        case AppScreen::MainMenu:
          switch (mainMenuSelected) {
            case 0: // Install
              currentScreen = AppScreen::Install;
              break;
            case 1: // Settings
              apiKeyInput = state.apiKey;
              mo2PathInput = state.mo2Path;
              currentScreen = AppScreen::Settings;
              break;
            case 2: // About
              currentScreen = AppScreen::About;
              break;
            case 3: // Exit
              screen.ExitLoopClosure()();
              break;
          }
          return true;

        case AppScreen::Install:
          if (!collectionUrl.empty() && !state.installing) {
            // Reset all state for new installation
            state.installing = true;
            state.phase = InstallPhase::Starting;
            state.totalMods = 0;
            state.toDownload = 0;
            state.downloading = 0;
            state.downloaded = 0;
            state.downloadFailed = 0;
            state.toInstall = 0;
            state.installed = 0;
            state.skipped = 0;
            state.failed = 0;
            state.hasError = false;
            state.logMessages.clear();

            currentScreen = AppScreen::Installing;

            // Start installation in background thread
            std::string url = collectionUrl;
            std::string mo2 = state.mo2Path;

            installThread = std::thread([&state, &screen, url, mo2]() {
              state.phase = InstallPhase::Starting;
              state.addLog("Starting installation...");
              state.addLog("Collection: " + url);
              state.addLog("MO2 Path: " + mo2);

              // Find NexusBridge executable
              std::string nexusBridge = findNexusBridge();
              if (nexusBridge.empty()) {
                state.addLog("ERROR: NexusBridge executable not found!");
                state.addLog("Make sure NexusBridge is in the same directory as NexusBridgeTUI");
                state.installing = false;
                screen.PostEvent(Event::Custom);
                return;
              }

              state.addLog("Using: " + nexusBridge);

              // Call NexusBridge CLI with --yes to auto-continue on failures
              // (popen doesn't allow stdin interaction, so we auto-continue)
              std::string cmd = "\"" + nexusBridge + "\" \"" + url + "\" \"" + mo2 + "\" --yes 2>&1";
              FILE* pipe = popen(cmd.c_str(), "r");
              if (pipe) {
                char buffer[512];
                while (fgets(buffer, sizeof(buffer), pipe)) {
                  std::string line(buffer);
                  if (!line.empty() && line.back() == '\n') {
                    line.pop_back();
                  }

                  // Parse phase changes and progress from CLI output
                  // "Mods: 488"
                  if (line.find("Mods:") != std::string::npos) {
                    try {
                      size_t pos = line.find("Mods:") + 5;
                      state.totalMods = std::stoi(line.substr(pos));
                    } catch (...) {}
                  }

                  // "=== Phase 1: Scanning archives ==="
                  if (line.find("Phase 1: Scanning") != std::string::npos) {
                    state.phase = InstallPhase::Scanning;
                  }

                  // "Need to download X archives"
                  if (line.find("Need to download") != std::string::npos) {
                    try {
                      size_t pos = line.find("Need to download") + 17;
                      size_t end = line.find(" archives");
                      if (end != std::string::npos) {
                        state.toDownload = std::stoi(line.substr(pos, end - pos));
                      }
                    } catch (...) {}
                  }

                  // "=== Phase 1b: Downloading X archives ==="
                  if (line.find("Phase 1b: Downloading") != std::string::npos) {
                    state.phase = InstallPhase::Downloading;
                    // Also extract the count from "Downloading X archives with Y threads"
                    try {
                      size_t pos = line.find("Downloading") + 12;
                      size_t end = line.find(" archives");
                      if (end != std::string::npos) {
                        state.toDownload = std::stoi(line.substr(pos, end - pos));
                      }
                    } catch (...) {}
                  }

                  // "[X/Y] Downloading: ModName" - individual download starts
                  // Count these as they appear to show progress during downloading
                  if (line.find("] Downloading:") != std::string::npos &&
                      line.find("[") != std::string::npos &&
                      line.find(" MB (") == std::string::npos) {
                    // This is a "[X/Y] Downloading: ModName" line, not a progress line
                    // Track how many downloads have started
                    state.downloading++;
                  }

                  // "Downloaded: X, Failed: Y" - summary after all downloads complete
                  if (line.find("Downloaded:") != std::string::npos &&
                      line.find("Failed:") != std::string::npos) {
                    try {
                      size_t pos = line.find("Downloaded:") + 12;
                      size_t end = line.find(",", pos);
                      if (end != std::string::npos) {
                        int dlCount = std::stoi(line.substr(pos, end - pos));
                        state.downloaded = dlCount;
                        state.downloading = dlCount;  // Sync downloading count too
                      }
                      // Also get failed count here
                      size_t failPos = line.find("Failed:") + 8;
                      std::string failStr = line.substr(failPos);
                      failStr.erase(0, failStr.find_first_not_of(" \t"));
                      int downloadFailedCount = std::stoi(failStr);
                      if (downloadFailedCount > 0) {
                        state.downloadFailed = downloadFailedCount;
                      }
                    } catch (...) {}
                  }

                  // "Downloaded: X" in final summary (without Failed: on same line)
                  if (line.find("Downloaded:") != std::string::npos &&
                      line.find("Failed:") == std::string::npos &&
                      line.find(",") == std::string::npos) {
                    try {
                      size_t pos = line.find("Downloaded:") + 11;
                      std::string numStr = line.substr(pos);
                      numStr.erase(0, numStr.find_first_not_of(" \t"));
                      size_t endPos = numStr.find_first_not_of("0123456789");
                      if (endPos != std::string::npos) {
                        numStr = numStr.substr(0, endPos);
                      }
                      int finalDownloaded = std::stoi(numStr);
                      state.downloaded = finalDownloaded;
                      state.downloading = finalDownloaded;
                    } catch (...) {}
                  }

                  // "=== Phase 2: Installing X mods ==="
                  if (line.find("Phase 2: Installing") != std::string::npos) {
                    state.phase = InstallPhase::Installing;
                    try {
                      size_t pos = line.find("Installing") + 11;
                      size_t end = line.find(" mods");
                      if (end != std::string::npos) {
                        state.toInstall = std::stoi(line.substr(pos, end - pos));
                      }
                    } catch (...) {}
                  }

                  // "[X/Y] ModName - Done!"
                  if (line.find("] ") != std::string::npos &&
                      line.find(" - Done!") != std::string::npos) {
                    state.installed++;
                  }

                  // "Installed:  X" in final summary (note: two spaces)
                  if (line.find("Installed:") != std::string::npos &&
                      line.find("/") == std::string::npos &&  // Not a progress line
                      line.find("Done!") == std::string::npos) {  // Not a completion line
                    try {
                      size_t pos = line.find("Installed:") + 10;
                      std::string numStr = line.substr(pos);
                      numStr.erase(0, numStr.find_first_not_of(" \t"));
                      // Remove any trailing text
                      size_t endPos = numStr.find_first_not_of("0123456789");
                      if (endPos != std::string::npos) {
                        numStr = numStr.substr(0, endPos);
                      }
                      int finalInstalled = std::stoi(numStr);
                      state.installed = finalInstalled;  // Use final count from summary
                    } catch (...) {}
                  }

                  // "Generating plugins.txt"
                  if (line.find("Generating plugins.txt") != std::string::npos ||
                      line.find("Generating modlist.txt") != std::string::npos) {
                    state.phase = InstallPhase::Generating;
                  }

                  // "Skipped: X (already installed)"
                  if (line.find("Skipped:") != std::string::npos &&
                      line.find("already installed") != std::string::npos) {
                    try {
                      size_t pos = line.find("Skipped:") + 8;
                      size_t end = line.find(" (");
                      if (end != std::string::npos) {
                        std::string numStr = line.substr(pos, end - pos);
                        // Trim whitespace
                        numStr.erase(0, numStr.find_first_not_of(" \t"));
                        state.skipped = std::stoi(numStr);
                      }
                    } catch (...) {}
                  }

                  // "Failed: X" in summary
                  if (line.find("Failed:") != std::string::npos &&
                      line.find("Downloaded:") == std::string::npos) {
                    try {
                      size_t pos = line.find("Failed:") + 7;
                      std::string numStr = line.substr(pos);
                      numStr.erase(0, numStr.find_first_not_of(" \t"));
                      int failed = std::stoi(numStr);
                      if (failed > 0) state.failed = failed;
                    } catch (...) {}
                  }

                  // "Done!" at end - but not the "[X/Y] ModName - Done!" lines
                  if (line.find("Done!") != std::string::npos &&
                      line.find(" - Done!") == std::string::npos) {
                    state.phase = InstallPhase::Complete;
                  }

                  // Also detect "Please restart Mod Organizer 2" as completion
                  if (line.find("restart Mod Organizer") != std::string::npos) {
                    state.phase = InstallPhase::Complete;
                  }

                  // Detect error lines
                  if (line.find("Error:") != std::string::npos ||
                      line.find("ERROR:") != std::string::npos) {
                    state.hasError = true;
                  }

                  state.addLog(line);
                  screen.PostEvent(Event::Custom);
                }
                pclose(pipe);
              } else {
                state.addLog("ERROR: Failed to start NexusBridge");
                state.hasError = true;
              }

              state.installing = false;
              // Set final phase based on whether there was an error
              if (state.hasError && state.phase != InstallPhase::Complete) {
                state.phase = InstallPhase::Error;
              } else if (state.phase != InstallPhase::Complete) {
                state.phase = InstallPhase::Complete;
              }
              screen.PostEvent(Event::Custom);
            });
            installThread.detach();
          }
          return true;

        case AppScreen::Installing:
          if (!state.installing) {
            currentScreen = AppScreen::MainMenu;
          }
          return true;

        case AppScreen::Settings:
          state.apiKey = apiKeyInput;
          state.mo2Path = mo2PathInput;
          saveSettings(state);
          state.addLog("Settings saved!");
          currentScreen = AppScreen::MainMenu;
          completions.clear();
          return true;

        case AppScreen::About:
          currentScreen = AppScreen::MainMenu;
          return true;
      }
      return true;
    }

    // Let the main component handle other events
    return false;
  });

  screen.Loop(component);

  return 0;
}
