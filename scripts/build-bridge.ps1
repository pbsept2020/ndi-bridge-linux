$ErrorActionPreference = "Stop"
Write-Host "=== NDI Bridge Windows - Full Setup ===" -ForegroundColor Green

# Step 1: Chocolatey
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Chocolatey..." -ForegroundColor Yellow
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-Expression ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
    $env:Path = "$env:ALLUSERSPROFILE\chocolatey\bin;$env:Path"
} else { Write-Host "Chocolatey OK" -ForegroundColor Green }

# Step 2: Git
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Git..." -ForegroundColor Yellow
    choco install git -y
    $env:Path = "C:\Program Files\Git\cmd;$env:Path"
} else { Write-Host "Git OK" -ForegroundColor Green }

# Step 3: CMake
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "Installing CMake..." -ForegroundColor Yellow
    choco install cmake --installargs 'ADD_CMAKE_TO_PATH=System' -y
    $env:Path = "C:\Program Files\CMake\bin;$env:Path"
} else { Write-Host "CMake OK" -ForegroundColor Green }

# Step 4: Visual Studio Build Tools
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    Write-Host "Installing VS Build Tools 2022 (this takes a while)..." -ForegroundColor Yellow
    choco install visualstudio2022buildtools --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" -y
} else { Write-Host "VS Build Tools OK" -ForegroundColor Green }

# Step 5: FFmpeg
$ffmpegDir = "C:\ffmpeg"
if (-not (Test-Path "$ffmpegDir\bin\ffmpeg.exe")) {
    Write-Host "Installing FFmpeg (with NVENC)..." -ForegroundColor Yellow
    $ffmpegUrl = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full-shared.7z"
    $ffmpegArchive = "$env:TEMP\ffmpeg.7z"
    Invoke-WebRequest -Uri $ffmpegUrl -OutFile $ffmpegArchive
    if (-not (Get-Command 7z -ErrorAction SilentlyContinue)) {
        choco install 7zip -y
        $env:Path = "C:\Program Files\7-Zip;$env:Path"
    }
    7z x $ffmpegArchive -o"$env:TEMP\ffmpeg-extract" -y
    $extractedDir = Get-ChildItem "$env:TEMP\ffmpeg-extract" -Directory | Select-Object -First 1
    if (Test-Path $ffmpegDir) { Remove-Item $ffmpegDir -Recurse -Force }
    Move-Item $extractedDir.FullName $ffmpegDir -Force
    $env:Path += ";$ffmpegDir\bin"
    [Environment]::SetEnvironmentVariable("Path", $env:Path, [EnvironmentVariableTarget]::Machine)
} else { Write-Host "FFmpeg OK" -ForegroundColor Green }

# Step 6: Check NDI SDK
$ndiDir = $null
@("C:\Program Files\NDI\NDI 6 SDK","C:\Program Files\NDI\NDI 5 SDK","C:\Program Files\NewTek\NDI 6 SDK","C:\Program Files\NewTek\NDI 5 SDK") | ForEach-Object {
    if ((Test-Path "$_\Include\Processing.NDI.Lib.h") -and -not $ndiDir) { $ndiDir = $_ }
}
if ($ndiDir) {
    Write-Host "NDI SDK: $ndiDir" -ForegroundColor Green
    $env:NDI_SDK_DIR = $ndiDir
} else {
    Write-Host "NDI SDK NOT FOUND - download from https://ndi.video/for-developers/ndi-sdk/" -ForegroundColor Red
    Write-Host "Install it, then re-run this script." -ForegroundColor Red
    exit 1
}

# Step 7: Clone repo
$repoDir = "$env:USERPROFILE\ndi-bridge-linux"
if (-not (Test-Path "$repoDir\.git")) {
    Write-Host "Cloning repo..." -ForegroundColor Yellow
    git clone https://github.com/pbsept2020/ndi-bridge-linux.git $repoDir
} else {
    Write-Host "Repo exists, pulling latest..." -ForegroundColor Yellow
    Push-Location $repoDir; git pull; Pop-Location
}

# Step 8: Build
Write-Host "Building NDI Bridge..." -ForegroundColor Yellow
Push-Location $repoDir
$ffmpegDir = "C:\ffmpeg"
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH="$ffmpegDir" -DAVCODEC_INCLUDE_DIRS="$ffmpegDir\include" -DAVUTIL_INCLUDE_DIRS="$ffmpegDir\include" -DSWSCALE_INCLUDE_DIRS="$ffmpegDir\include" -DAVCODEC_LIBRARY_DIRS="$ffmpegDir\lib" -DAVUTIL_LIBRARY_DIRS="$ffmpegDir\lib" -DSWSCALE_LIBRARY_DIRS="$ffmpegDir\lib" -DAVCODEC_LIBRARIES="avcodec" -DAVUTIL_LIBRARIES="avutil" -DSWSCALE_LIBRARIES="swscale" -DAVCODEC_FOUND=TRUE -DAVUTIL_FOUND=TRUE -DSWSCALE_FOUND=TRUE
cmake --build build --config Release
Pop-Location

Write-Host "`n=== DONE ===" -ForegroundColor Green
Write-Host "Binary: $repoDir\build\Release\ndi-bridge-x.exe" -ForegroundColor Cyan
Write-Host "Test:   .\build\Release\ndi-bridge-x.exe join --name `"Test`" --port 5990 -v" -ForegroundColor Cyan
