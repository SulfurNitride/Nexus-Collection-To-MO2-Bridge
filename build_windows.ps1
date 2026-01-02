# NexusBridge Windows Build Script
# Requires: Visual Studio 2022, Rust, .NET 10 SDK, CMake, vcpkg (at C:\vcpkg)

param(
    [switch]$Clean,
    [switch]$SkipRust,
    [switch]$SkipCpp,
    [switch]$SkipGui,
    [switch]$Package
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot

Write-Host "=== NexusBridge Windows Build ===" -ForegroundColor Cyan

# Clean if requested
if ($Clean) {
    Write-Host "`n[1/6] Cleaning previous builds..." -ForegroundColor Yellow
    if (Test-Path "$ProjectRoot\build") { Remove-Item -Recurse -Force "$ProjectRoot\build" }
    if (Test-Path "$ProjectRoot\NexusBridge-Windows-x64") { Remove-Item -Recurse -Force "$ProjectRoot\NexusBridge-Windows-x64" }
    if (Test-Path "$ProjectRoot\external\libloot\target") { Remove-Item -Recurse -Force "$ProjectRoot\external\libloot\target" }
    if (Test-Path "$ProjectRoot\NexusBridgeGui\bin") { Remove-Item -Recurse -Force "$ProjectRoot\NexusBridgeGui\bin" }
    if (Test-Path "$ProjectRoot\NexusBridgeGui\obj") { Remove-Item -Recurse -Force "$ProjectRoot\NexusBridgeGui\obj" }
}

# Step 1: Build libloot (Rust)
if (-not $SkipRust) {
    Write-Host "`n[1/6] Building libloot (Rust)..." -ForegroundColor Yellow
    Push-Location "$ProjectRoot\external\libloot"
    try {
        cargo build --release -p libloot -p libloot-cpp
        if ($LASTEXITCODE -ne 0) { throw "Rust build failed" }
    } finally {
        Pop-Location
    }
    Write-Host "  libloot built successfully" -ForegroundColor Green
} else {
    Write-Host "`n[1/6] Skipping libloot build" -ForegroundColor DarkGray
}

# Step 2: Install vcpkg dependencies if needed
Write-Host "`n[2/6] Checking vcpkg dependencies..." -ForegroundColor Yellow
$vcpkgRoot = "C:\vcpkg"
if (-not (Test-Path "$vcpkgRoot\installed\x64-windows-static\lib\libcurl.lib")) {
    Write-Host "  Installing curl via vcpkg..."
    & "$vcpkgRoot\vcpkg" install curl:x64-windows-static
    if ($LASTEXITCODE -ne 0) { throw "vcpkg curl install failed" }
}
Write-Host "  vcpkg dependencies ready" -ForegroundColor Green

# Step 3: Build NexusBridge CLI (CMake)
if (-not $SkipCpp) {
    Write-Host "`n[3/6] Building NexusBridge CLI (C++)..." -ForegroundColor Yellow

    if (-not (Test-Path "$ProjectRoot\build")) {
        New-Item -ItemType Directory -Path "$ProjectRoot\build" | Out-Null
    }

    Push-Location "$ProjectRoot\build"
    try {
        cmake .. -G "Visual Studio 17 2022" -A x64 `
            -DCMAKE_BUILD_TYPE=Release `
            -DVCPKG_TARGET_TRIPLET=x64-windows-static `
            -DCMAKE_TOOLCHAIN_FILE="$vcpkgRoot\scripts\buildsystems\vcpkg.cmake"
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

        cmake --build . --config Release --target NexusBridge
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
    } finally {
        Pop-Location
    }
    Write-Host "  NexusBridge CLI built successfully" -ForegroundColor Green
} else {
    Write-Host "`n[3/6] Skipping C++ build" -ForegroundColor DarkGray
}

# Step 4: Build NexusBridgeGui (.NET)
if (-not $SkipGui) {
    Write-Host "`n[4/6] Building NexusBridgeGui (.NET)..." -ForegroundColor Yellow
    Push-Location "$ProjectRoot\NexusBridgeGui"
    try {
        dotnet publish -c Release -r win-x64 --self-contained `
            -p:PublishSingleFile=true `
            -p:IncludeNativeLibrariesForSelfExtract=true `
            -o "$ProjectRoot\publish\windows"
        if ($LASTEXITCODE -ne 0) { throw ".NET build failed" }
    } finally {
        Pop-Location
    }
    Write-Host "  NexusBridgeGui built successfully" -ForegroundColor Green
} else {
    Write-Host "`n[4/6] Skipping GUI build" -ForegroundColor DarkGray
}

# Step 5: Download full 7-Zip (with RAR support) if needed
Write-Host "`n[5/6] Getting 7-Zip..." -ForegroundColor Yellow
$sevenZipExe = "$ProjectRoot\7z.exe"
$sevenZipDll = "$ProjectRoot\7z.dll"
if (-not (Test-Path $sevenZipExe) -or -not (Test-Path $sevenZipDll)) {
    Write-Host "  Downloading full 7-Zip (with RAR support)..."
    $sevenZipUrl = "https://www.7-zip.org/a/7z2408-x64.exe"
    $sevenZipInstaller = "$ProjectRoot\7z-installer.exe"

    Invoke-WebRequest -Uri $sevenZipUrl -OutFile $sevenZipInstaller

    # Use system 7z to extract the installer (it's a 7z archive)
    $system7z = "C:\Program Files\7-Zip\7z.exe"
    if (Test-Path $system7z) {
        & $system7z x $sevenZipInstaller -o"$ProjectRoot\7zip_full" -y | Out-Null
    } else {
        throw "Please install 7-Zip to extract the 7-Zip bundle, or manually place 7z.exe and 7z.dll in project root"
    }

    Copy-Item "$ProjectRoot\7zip_full\7z.exe" $sevenZipExe
    Copy-Item "$ProjectRoot\7zip_full\7z.dll" $sevenZipDll
    Remove-Item $sevenZipInstaller -ErrorAction SilentlyContinue
    Remove-Item "$ProjectRoot\7zip_full" -Recurse -ErrorAction SilentlyContinue
}
Write-Host "  7z.exe + 7z.dll ready" -ForegroundColor Green

# Step 6: Package distribution
if ($Package) {
    Write-Host "`n[6/6] Packaging distribution..." -ForegroundColor Yellow

    $distDir = "$ProjectRoot\NexusBridge-Windows-x64"
    if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }
    New-Item -ItemType Directory -Path $distDir | Out-Null

    # Copy CLI
    Copy-Item "$ProjectRoot\build\Release\NexusBridge.exe" "$distDir\"

    # Copy GUI (single-file self-contained exe)
    $guiExe = "$ProjectRoot\publish\windows\NexusBridgeGui.exe"
    if (Test-Path $guiExe) {
        Copy-Item $guiExe "$distDir\"
    } else {
        Write-Host "  WARNING: NexusBridgeGui.exe not found - run without -SkipGui" -ForegroundColor Red
    }

    # Copy libloot.dll
    $lootDll = Get-ChildItem -Path "$ProjectRoot\external\libloot\target\release" -Filter "*.dll" |
               Where-Object { $_.Name -match "loot" } |
               Select-Object -First 1
    if ($lootDll) {
        Copy-Item $lootDll.FullName "$distDir\libloot.dll"
    }

    # Copy 7z.exe and 7z.dll
    Copy-Item $sevenZipExe "$distDir\"
    Copy-Item $sevenZipDll "$distDir\"

    # Create zip
    $zipPath = "$ProjectRoot\NexusBridge-Windows-x64.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    Compress-Archive -Path "$distDir\*" -DestinationPath $zipPath

    Write-Host "  Distribution packaged: $zipPath" -ForegroundColor Green
    Write-Host "`n  Contents:" -ForegroundColor Cyan
    Get-ChildItem $distDir | ForEach-Object { Write-Host "    - $($_.Name)" }
} else {
    Write-Host "`n[6/6] Skipping packaging (use -Package to create NexusBridge-Windows-x64)" -ForegroundColor DarkGray
}

Write-Host "`n=== Build Complete ===" -ForegroundColor Cyan
Write-Host "Outputs:"
Write-Host "  CLI: $ProjectRoot\build\Release\NexusBridge.exe"
Write-Host "  GUI: $ProjectRoot\publish\windows\NexusBridgeGui.exe"
if ($Package) {
    Write-Host "  Dir: $ProjectRoot\NexusBridge-Windows-x64"
    Write-Host "  Zip: $ProjectRoot\NexusBridge-Windows-x64.zip"
}
