# YouCanSingWell 의존성 설치 스크립트 (Windows / PowerShell)
# third_party/ 아래에 빌드에 필요한 라이브러리를 받아 놓는다. 저장소에는 포함하지 않는다.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup-deps.ps1
#
# 필요: git, curl (Windows 10+ 기본 포함), Visual Studio 2022 (libmpv import lib 생성용 lib.exe)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tp = Join-Path $root "third_party"
New-Item -ItemType Directory -Force $tp | Out-Null
Set-Location $tp

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# 1. 단일 헤더 라이브러리
if (-not (Test-Path "miniaudio.h")) {
    Step "miniaudio.h"
    curl.exe -sL -o miniaudio.h "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"
}
if (-not (Test-Path "json.hpp")) {
    Step "nlohmann/json.hpp"
    curl.exe -sL -o json.hpp "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp"
}

# 2. SQLite amalgamation
if (-not (Test-Path "sqlite\sqlite3.c")) {
    Step "SQLite amalgamation"
    curl.exe -sL -o sqlite.zip "https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip"
    Expand-Archive -Path sqlite.zip -DestinationPath . -Force
    Remove-Item sqlite.zip
    Rename-Item "sqlite-amalgamation-3530400" "sqlite"
}

# 3. GLFW 3.4 (MSVC 바이너리)
if (-not (Test-Path "glfw\lib-vc2022\glfw3.lib")) {
    Step "GLFW 3.4"
    curl.exe -sL -o glfw.zip "https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.bin.WIN64.zip"
    Expand-Archive -Path glfw.zip -DestinationPath . -Force
    Remove-Item glfw.zip
    if (Test-Path "glfw") { Remove-Item glfw -Recurse -Force }
    Rename-Item "glfw-3.4.bin.WIN64" "glfw"
}

# 4. Dear ImGui (검증된 커밋에 고정)
if (-not (Test-Path "imgui\imgui.h")) {
    Step "Dear ImGui"
    git clone -q https://github.com/ocornut/imgui.git imgui
    git -C imgui checkout -q ea6d21687bec144dd7aee0f4db37f7c61a8799bb
}

# 5. libmpv (zhongfly/mpv-winbuild dev 패키지) + MSVC import lib 생성 — 노래 영상 재생용
if (-not (Test-Path "mpv\libmpv.lib")) {
    Step "libmpv"
    New-Item -ItemType Directory -Force mpv | Out-Null
    $mpvUrl = "https://github.com/zhongfly/mpv-winbuild/releases/download/2026-09-09-7e4cb538a3/mpv-dev-x86_64-20260909-git-7e4cb538a3.7z"
    curl.exe -sL -o mpv-dev.7z $mpvUrl
    & "$env:SystemRoot\System32\tar.exe" -xf mpv-dev.7z -C mpv
    Remove-Item mpv-dev.7z

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vswhere -latest -products * -property installationPath
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    Set-Location mpv
    $exports = cmd /c "`"$vcvars`" >nul 2>&1 && dumpbin /exports libmpv-2.dll"
    $names = $exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(mpv_\w+)') { $matches[1] } }
    "LIBRARY libmpv-2.dll`nEXPORTS`n" + ($names -join "`n") | Out-File -Encoding ascii libmpv.def
    cmd /c "`"$vcvars`" >nul 2>&1 && lib /nologo /def:libmpv.def /machine:x64 /out:libmpv.lib"
    Set-Location $tp
}

# 6. demucs.cpp (보컬 분리 추론 라이브러리, MIT) + Eigen 서브모듈
if (-not (Test-Path "demucs.cpp\src\model.hpp")) {
    Step "demucs.cpp"
    git clone -q --depth 1 https://github.com/sevagh/demucs.cpp.git demucs.cpp
}
if (-not (Test-Path "demucs.cpp\vendor\eigen\Eigen\Core")) {
    Step "Eigen (demucs.cpp 서브모듈)"
    git -C demucs.cpp submodule update --init --depth 1 vendor/eigen
}

# 7. 동봉 도구: yt-dlp, ffmpeg, deno (yt-dlp 의 자바스크립트 실행용) → build\Release\bin
$bin = Join-Path $root "build\Release\bin"
New-Item -ItemType Directory -Force $bin | Out-Null
if (-not (Test-Path "$bin\yt-dlp.exe")) {
    Step "yt-dlp"
    curl.exe -sL -o "$bin\yt-dlp.exe" "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"
}
if (-not (Test-Path "$bin\ffmpeg.exe")) {
    Step "ffmpeg"
    curl.exe -sL -o ffmpeg.zip "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
    Expand-Archive -Path ffmpeg.zip -DestinationPath ffmpeg-tmp -Force
    Copy-Item (Get-ChildItem ffmpeg-tmp -Recurse -Filter ffmpeg.exe | Select-Object -First 1).FullName "$bin\ffmpeg.exe"
    Remove-Item ffmpeg.zip; Remove-Item ffmpeg-tmp -Recurse -Force
}
if (-not (Test-Path "$bin\deno.exe")) {
    Step "deno"
    curl.exe -sL -o deno.zip "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip"
    Expand-Archive -Path deno.zip -DestinationPath deno-tmp -Force
    Copy-Item "deno-tmp\deno.exe" "$bin\deno.exe"
    Remove-Item deno.zip; Remove-Item deno-tmp -Recurse -Force
}

Write-Host ""
Write-Host "완료. 이제 빌드하세요:" -ForegroundColor Green
Write-Host '  cmake -S . -B build -A x64'
Write-Host '  cmake --build build --config Release'
