#pragma once

#include <atomic>
#include <functional>
#include <string>

// Progress callback for TUI integration
struct InstallProgress {
  std::atomic<int> totalMods{0};
  std::atomic<int> currentMod{0};
  std::atomic<int> downloaded{0};
  std::atomic<int> installed{0};
  std::atomic<int> skipped{0};
  std::atomic<int> failed{0};
  std::atomic<bool> cancelled{false};

  // Callback for log messages
  std::function<void(const std::string&)> logCallback;

  void log(const std::string& msg) {
    if (logCallback) {
      logCallback(msg);
    }
  }
};

// Main installation function - can be called from TUI or CLI
// Returns 0 on success, non-zero on error
int installCollection(
    const std::string& collectionInput,  // URL or path to collection.json
    const std::string& mo2Path,           // Path to MO2 directory
    const std::string& apiKey,            // Nexus API key (empty to load from file)
    InstallProgress* progress = nullptr   // Optional progress tracking
);

// Validate API key and return username (empty on failure)
std::string validateApiKey(const std::string& apiKey);

// Check if API key has premium
bool checkPremium(const std::string& apiKey);
