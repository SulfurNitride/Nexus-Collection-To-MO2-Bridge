# Nexus Collection to MO2 Bridge

A lightweight, standalone bridge tool to install Nexus Collections directly into Mod Organizer 2, bypassing Vortex completely.

# [DISCORD](https://discord.gg/9JWQzSeUWt)

# [NEXUS RELEASE](https://www.nexusmods.com/site/mods/1563)

## Features
*   Direct Install, this is **MEANT** for a clean install of MO2, and only supports Skyrim as of right now! Feel free to test other games but I'm just looking at skyrim.
*   Everything is already bundled in you need (including 7-Zip and Loot). (Windows users might need VCRUN2022)
*   Compatibile on both Windows and Linux.
*   I've tried my best to match the sorting algorithm that Vortex uses, so at least in my testing Plugins.txt is anywhere from a 90-100% match as if vortex did it. But the modlist.txt for mod conflicts is still a WIP!!! This means some things could be messed up!!!!!
*   [Immersive and Pure](https://www.nexusmods.com/games/skyrimspecialedition/collections/qfftpq) is the collection I was able to install without any issues. If you do have issues with modlists I.E wrong versions of mods installed please let me know either through github issues, or the discord! Please let me know the platform you are on, and what collection you are trying to install.

## Usage
1.  **Collection URL:** Paste the link to the Nexus Collection you want to install.
2.  **Directories:** Select your MO2 instance folder.
3.  The tool will handle the rest!

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

    Gen'd with support and help with Claude Code. I will eventually be rewritting this in Rust, and try to make it all by myself at some point if possible. If you want to follow my progress of me learning rust I just started [here.](https://www.youtube.com/watch?v=IqGkaZvdcjk&list=PLB52iltb95qIMAQNma2nHP7Og9c9mqAXl&pp=gAQB)
