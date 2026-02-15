# setup-windows.ps1 — Install build tools and compile NDI Bridge on Windows
# Run this in PowerShell (Admin) on Vmix1-frankfurt after RDP connect
#
# Prerequisites: Windows Server with NVIDIA L4 GPU
# This script installs: Git, CMake, Visual Studio Build Tools, FFmpeg (with NVENC), NDI SDK
# Then clones the repo and builds.

$ErrorActionPreference = "Stop"

Write-Host "=== NDI Bridge Windows Setup ===" -ForegroundColor Green
Write-Host "Instance: Vmix1-frankfurt (g6.xlarge, NVIDIA L4)" -ForegroundColor Cyan

# --- Step 1: Check NVIDIA GPU ---
Write-Host "`n[1/6] Checking NVIDIA GPU..." -ForegroundColor Yellow
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
    nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader
} else {
    Write-Host "WARNING: nvidia-smi not found. Install NVIDIA drivers first:" -ForegroundColor Red
    Write-Host "  https://www.nvidia.com/Download/index.aspx (L4 / Windows Server)" -ForegroundColor Red
    Write-Host "  Or use: choco install nvidia-display-driver" -ForegroundColor Red
}

# --- Step 2: Install Chocolatey (package manager) ---
Write-Host "`n[2/6] Installing Chocolatey..." -ForegroundColor Yellow
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-Expression ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
    refreshenv
} else {
    Write-Host "Chocolatey already installed" -ForegroundColor Green
}

# --- Step 3: Install build tools ---
Write-Host "`n[3/6] Installing Git, CMake, Visual Studio Build Tools..." -ForegroundColor Yellow
choco install git cmake --installargs 'ADD_CMAKE_TO_PATH=System' -y
choco install visualstudio2022buildtools --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" -y

# --- Step 4: Install FFmpeg with NVENC ---
Write-Host "`n[4/6] Installing FFmpeg (with NVENC support)..." -ForegroundColor Yellow
$ffmpegDir = "C:\ffmpeg"
if (-not (Test-Path "$ffmpegDir\bin\ffmpeg.exe")) {
    # Download gyan.dev full build (includes NVENC, QSV, AV1)
    $ffmpegUrl = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full-shared.7z"
    $ffmpegArchive = "$env:TEMP\ffmpeg.7z"

    Write-Host "Downloading FFmpeg from gyan.dev..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $ffmpegUrl -OutFile $ffmpegArchive

    # Need 7-Zip to extract
    if (-not (Get-Command 7z -ErrorAction SilentlyContinue)) {
        choco install 7zip -y
        refreshenv
    }

    # Extract
    7z x $ffmpegArchive -o"$env:TEMP\ffmpeg-extract" -y
    $extractedDir = Get-ChildItem "$env:TEMP\ffmpeg-extract" -Directory | Select-Object -First 1
    Move-Item $extractedDir.FullName $ffmpegDir -Force

    # Add to PATH
    $env:Path += ";$ffmpegDir\bin"
    [Environment]::SetEnvironmentVariable("Path", $env:Path, [EnvironmentVariableTarget]::Machine)

    Write-Host "FFmpeg installed to $ffmpegDir" -ForegroundColor Green
} else {
    Write-Host "FFmpeg already installed at $ffmpegDir" -ForegroundColor Green
}

# Verify NVENC support
Write-Host "Checking NVENC support..." -ForegroundColor Cyan
& "$ffmpegDir\bin\ffmpeg.exe" -hide_banner -encoders 2>$null | Select-String "h264_nvenc|av1_nvenc"

# --- Step 5: Check NDI SDK ---
Write-Host "`n[5/6] Checking NDI SDK..." -ForegroundColor Yellow
$ndiPaths = @(
    "C:\Program Files\NDI\NDI 6 SDK",
    "C:\Program Files\NDI\NDI 5 SDK",
    "C:\Program Files\NewTek\NDI 6 SDK",
    "C:\Program Files\NewTek\NDI 5 SDK"
)
$ndiFound = $false
foreach ($p in $ndiPaths) {
    if (Test-Path "$p\Include\Processing.NDI.Lib.h") {
        Write-Host "NDI SDK found at: $p" -ForegroundColor Green
        [Environment]::SetEnvironmentVariable("NDI_SDK_DIR", $p, [EnvironmentVariableTarget]::Machine)
        $env:NDI_SDK_DIR = $p
        $ndiFound = $true
        break
    }
}
if (-not $ndiFound) {
    Write-Host "NDI SDK NOT FOUND. Download and install from:" -ForegroundColor Red
    Write-Host "  https://ndi.video/for-developers/ndi-sdk/" -ForegroundColor Red
    Write-Host "  Then re-run this script." -ForegroundColor Red
}

# --- Step 6: Clone and build ---
Write-Host "`n[6/6] Cloning and building NDI Bridge..." -ForegroundColor Yellow
$repoDir = "$env:USERPROFILE\ndi-bridge-linux"

if (-not (Test-Path $repoDir)) {
    Write-Host "NOTE: Clone the repo manually or copy files via SCP/RDP." -ForegroundColor Cyan
    Write-Host "  Option A (if git configured): git clone <your-repo-url> $repoDir" -ForegroundColor Cyan
    Write-Host "  Option B: Copy from Mac via shared folder or SCP" -ForegroundColor Cyan
} else {
    Write-Host "Repo found at $repoDir" -ForegroundColor Green
}

if (Test-Path $repoDir) {
    Push-Location $repoDir

    # Set FFmpeg paths for CMake (pkg-config doesn't exist on Windows, use manual paths)
    $env:FFMPEG_DIR = $ffmpegDir

    Write-Host "Configuring with CMake..." -ForegroundColor Cyan
    # Use CMake with manual FFmpeg paths since pkg-config isn't available on Windows
    cmake -B build -G "Visual Studio 17 2022" `
        -DCMAKE_PREFIX_PATH="$ffmpegDir" `
        -DAVCODEC_INCLUDE_DIRS="$ffmpegDir\include" `
        -DAVUTIL_INCLUDE_DIRS="$ffmpegDir\include" `
        -DSWSCALE_INCLUDE_DIRS="$ffmpegDir\include" `
        -DAVCODEC_LIBRARY_DIRS="$ffmpegDir\lib" `
        -DAVUTIL_LIBRARY_DIRS="$ffmpegDir\lib" `
        -DSWSCALE_LIBRARY_DIRS="$ffmpegDir\lib" `
        -DAVCODEC_LIBRARIES="avcodec" `
        -DAVUTIL_LIBRARIES="avutil" `
        -DSWSCALE_LIBRARIES="swscale" `
        -DAVCODEC_FOUND=TRUE `
        -DAVUTIL_FOUND=TRUE `
        -DSWSCALE_FOUND=TRUE

    Write-Host "Building Release..." -ForegroundColor Cyan
    cmake --build build --config Release

    Write-Host "`n=== Build complete ===" -ForegroundColor Green
    Write-Host "Binaries in: $repoDir\build\Release\" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Test commands:" -ForegroundColor Yellow
    Write-Host "  .\build\Release\ndi-bridge.exe join --name `"Pierre Frankfurt`" --port 5990 -v"
    Write-Host "  .\build\Release\ndi-bridge.exe host --source `"Pierre Frankfurt`" --target 192.168.1.9:5991 -v"

    Pop-Location
} else {
    Write-Host "`nAfter copying the repo, run:" -ForegroundColor Yellow
    Write-Host "  cd $repoDir" -ForegroundColor Cyan
    Write-Host "  cmake -B build -G `"Visual Studio 17 2022`"" -ForegroundColor Cyan
    Write-Host "  cmake --build build --config Release" -ForegroundColor Cyan
}

Write-Host "`n=== Setup complete ===" -ForegroundColor Green
Write-Host "Cost reminder: g6.xlarge ~ `$0.80/h — stop the instance when done!" -ForegroundColor Red
Write-Host "  aws ec2 stop-instances --instance-ids i-0a9313dae0af02d4b" -ForegroundColor Red
