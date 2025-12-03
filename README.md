# Nexus Collection to MO2 Bridge

A lightweight, standalone bridge tool to install Nexus Collections directly into Mod Organizer 2 (MO2), bypassing Vortex completely.

## Features
*   **Direct Installation:** Installs collections directly to your MO2 `mods` folder.
*   **Zero Dependencies:** Bundles everything you need (including 7-Zip and Loot). No need to install Python, Node.js, or other runtimes.
*   **FOMOD Support:** Automatically handles FOMOD installers with the correct options selected by the collection curator.
*   **Cross-Platform:** Native support for Windows and Linux.

## Installation

### Windows
1.  Go to the [Releases](https://github.com/SulfurNitride/Nexus-Collection-To-MO2-Bridge/releases) page.
2.  Download `NexusBridge-Windows-x64.zip`.
3.  Extract the zip file anywhere.
4.  Run `NexusBridge.exe`.

### Linux
1.  Go to the [Releases](https://github.com/SulfurNitride/Nexus-Collection-To-MO2-Bridge/releases) page.
2.  Download `NexusBridge-Linux-x64.tar.gz`.
3.  Extract the archive.
4.  Run `./NexusBridge`.

*Note: Linux users may need to install `libcurl4` if not already present (standard on most distros).*

## Usage
1.  **API Key:** On first run, you will be asked for your Nexus Mods API Key (Premium required for automatic downloads).
2.  **Collection URL:** Paste the link to the Nexus Collection you want to install.
3.  **Directories:** Select your MO2 instance folder.
4.  The tool will handle the rest!

## Building from Source

If you want to build the tool yourself instead of using the pre-compiled releases:

### Prerequisites
*   **CMake** (3.14 or newer)
*   **C++ Compiler** (MSVC on Windows, GCC/Clang on Linux)
*   **Rust** (Required to build libloot)
*   **libcurl** (development headers)

### Build Instructions

1.  Clone the repository:
    ```bash
    git clone https://github.com/SulfurNitride/Nexus-Collection-To-MO2-Bridge.git
    cd NexusBridge
    ```

2.  Create a build directory and compile:
    ```bash
    mkdir build && cd build
    cmake ..
    cmake --build . --config Release
    ```