# setup_windows.ps1 — Scholion Windows build environment setup
# Run once in a PowerShell window (right-click → "Run as Administrator" for MSYS2 install)
#
# Usage:  powershell -ExecutionPolicy Bypass -File setup_windows.ps1
#
# This script installs MSYS2, then uses it to install all build dependencies.
# After it completes, jump to the BUILD section at the bottom.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Write-Host "=== Scholion Windows Build Setup ===" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# 1. Install MSYS2 (if not already installed)
# ---------------------------------------------------------------------------
$msys2 = "C:\msys64"
if (-not (Test-Path "$msys2\usr\bin\bash.exe")) {
    Write-Host "`n[1/5] Downloading MSYS2 installer..." -ForegroundColor Yellow
    $installer = "$env:TEMP\msys2-installer.exe"
    Invoke-WebRequest -Uri "https://github.com/msys2/msys2-installer/releases/download/2024-01-13/msys2-x86_64-20240113.exe" `
                      -OutFile $installer
    Write-Host "[1/5] Installing MSYS2 to C:\msys64 ..."
    Start-Process -FilePath $installer -ArgumentList "install --confirm-command --accept-messages --root C:\msys64" -Wait
} else {
    Write-Host "[1/5] MSYS2 already installed at $msys2" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 2. Install MinGW-w64 toolchain + dependencies via pacman
# ---------------------------------------------------------------------------
Write-Host "`n[2/5] Installing MinGW-w64 toolchain and dependencies..." -ForegroundColor Yellow

$packages = @(
    "mingw-w64-x86_64-toolchain",   # gcc, g++, ld, etc.
    "mingw-w64-x86_64-cmake",       # CMake
    "mingw-w64-x86_64-ninja",       # Ninja build system (faster than make)
    "mingw-w64-x86_64-glfw",        # GLFW
    "mingw-w64-x86_64-curl",        # libcurl
    "mingw-w64-x86_64-opengl-headers" # OpenGL headers
)

$pkgList = $packages -join " "
& "$msys2\usr\bin\bash.exe" -lc "pacman -S --noconfirm --needed $pkgList"

Write-Host "[2/5] Packages installed" -ForegroundColor Green

# ---------------------------------------------------------------------------
# 3. Verify MuPDF libs are present
# ---------------------------------------------------------------------------
Write-Host "`n[3/5] Checking MuPDF libraries..." -ForegroundColor Yellow

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$mupdfDir = Join-Path $scriptDir "third_party\mupdf\lib"
$mupdf    = Join-Path $mupdfDir "mupdf.lib"
$mupdfThird = Join-Path $mupdfDir "mupdf-third.lib"

if (-not (Test-Path $mupdf) -or -not (Test-Path $mupdfThird)) {
    Write-Host @"

[3/5] MuPDF Windows libs NOT found at:
    $mupdfDir\mupdf.lib
    $mupdfDir\mupdf-third.lib

See third_party/mupdf/lib/PLACE_WINDOWS_LIBS_HERE.txt for instructions.
You can still build WITHOUT MuPDF (no PDF rendering) by omitting -DSCHOLION_WITH_MUPDF=ON
"@ -ForegroundColor Red
} else {
    Write-Host "[3/5] MuPDF libs found" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 4. Verify GLAD is present
# ---------------------------------------------------------------------------
Write-Host "`n[4/5] Checking GLAD..." -ForegroundColor Yellow

$gladSrc = Join-Path $scriptDir "third_party\glad\src\glad.c"
if (Test-Path $gladSrc) {
    Write-Host "[4/5] GLAD source found" -ForegroundColor Green
} else {
    Write-Host "[4/5] GLAD source NOT found at $gladSrc`n  See third_party/glad/SETUP_GLAD.md" -ForegroundColor Red
}

# ---------------------------------------------------------------------------
# 5. Configure and build
# ---------------------------------------------------------------------------
Write-Host "`n[5/5] Configuring CMake build..." -ForegroundColor Yellow

$buildDir = Join-Path $scriptDir "build"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

$mingwPath = "C:\msys64\mingw64\bin"
$env:PATH = "$mingwPath;$env:PATH"

$hasMuPDF = (Test-Path $mupdf)
$mupdfFlag = if ($hasMuPDF) { "-DSCHOLION_WITH_MUPDF=ON" } else { "" }

Push-Location $buildDir
try {
    & "$mingwPath\cmake.exe" .. `
        -G "Ninja" `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_C_COMPILER="$mingwPath\gcc.exe" `
        -DCMAKE_CXX_COMPILER="$mingwPath\g++.exe" `
        $mupdfFlag

    Write-Host "`nBuilding Scholion..." -ForegroundColor Yellow
    & "$mingwPath\ninja.exe"
} finally {
    Pop-Location
}

Write-Host @"

=== Build complete ===
Executable: $buildDir\Scholion.exe

To run: .\build\Scholion.exe
"@ -ForegroundColor Cyan
