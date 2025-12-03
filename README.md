# Nexus Collection to MO2 Bridge

A bridge tool to simplify managing mod collections between Vortex and Mod Organizer 2 (MO2).

## Building

### Prerequisites
*   **CMake** (3.14 or newer)
*   **C++ Compiler** (GCC/Clang supporting C++17)
*   **Rust** (Required to build libloot)
*   **libcurl** (development headers)

### Build Instructions

1.  Clone the repository:
    ```bash
    git clone <repo_url>
    cd NexusBridge
    ```

2.  Create a build directory and compile:
    ```bash
    mkdir build && cd build
    cmake ..
    make
    ```

3.  Run the tool:
    ```bash
    ./NexusBridge
    ```

## Structure
*   `src/`: Core C++ source code.
*   `external/`: Bundled dependencies (libloot).
