#!/bin/bash
set -e

cd "$(dirname "$0")"

echo "=== Building NexusBridge ==="

# Build libloot first
echo "Building libloot..."
cd external/libloot
cargo build --release -p libloot -p libloot-cpp
cd ../..

# Build CLI
echo "Building NexusBridge CLI..."
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
cd ..

# Build GUI
echo "Building NexusBridgeGui..."
cd NexusBridgeGui
dotnet publish -c Release -r linux-x64 --self-contained -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o ../dist
cd ..

# Copy CLI and libloot to dist
echo "Copying files to dist..."
cp build/NexusBridge dist/
# Find libloot and create symlinks for both possible names
LOOT_LIB=$(find external/libloot/target/release -name "lib*loot.so" | head -1)
if [ -n "$LOOT_LIB" ]; then
  LOOT_NAME=$(basename "$LOOT_LIB")
  cp "$LOOT_LIB" "dist/$LOOT_NAME"
  cd dist
  [ "$LOOT_NAME" != "libloot.so" ] && ln -sf "$LOOT_NAME" libloot.so
  [ "$LOOT_NAME" != "liblibloot.so" ] && ln -sf "$LOOT_NAME" liblibloot.so
  cd ..
fi

# Download 7zip if not present
if [ ! -f dist/7zzs ]; then
    echo "Downloading 7-Zip..."
    cd dist
    wget -q https://www.7-zip.org/a/7z2408-linux-x64.tar.xz
    tar -xf 7z2408-linux-x64.tar.xz 7zzs
    rm 7z2408-linux-x64.tar.xz
    cd ..
fi

echo ""
echo "=== Build Complete ==="
echo "Output in dist/:"
ls -la dist/
