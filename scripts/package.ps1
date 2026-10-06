# YouCanSingWell 배포 패키지 만들기
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1 [-SkipBuild] [-SkipInstaller]
#
# 결과: dist\YouCanSingWell-v<버전>-win64.zip (포터블), dist\YouCanSingWell-Setup-v<버전>.exe (Inno Setup 설치 프로그램)
# 동봉: youcansingwell.exe → YouCanSingWell.exe, ycs_separate.exe (보컬 분리), bin\yt-dlp.exe, bin\ffmpeg.exe, bin\deno.exe
# 보컬 분리 모델(약 80MB) 은 앱이 처음 노래를 가져올 때 받는다 (패키지에 넣지 않음).

param([switch]$SkipBuild, [switch]$SkipInstaller)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(YouCanSingWell VERSION ([0-9.]+)').Matches[0].Groups[1].Value
Write-Host "==> YouCanSingWell v$version" -ForegroundColor Cyan

if (-not $SkipBuild) {
    Write-Host "==> 빌드" -ForegroundColor Cyan
    cmake -S . -B build -A x64
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build build --config Release --target youcansingwell --target ycs_separate
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$dist = Join-Path $root "dist"
$cache = Join-Path $dist "cache"
$stage = Join-Path $dist "YouCanSingWell"
New-Item -ItemType Directory -Force $cache | Out-Null
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\bin" | Out-Null

function Fetch($name, $url) {
    $f = Join-Path $cache $name
    if (-not (Test-Path $f)) {
        Write-Host "==> 다운로드 $name" -ForegroundColor Cyan
        curl.exe -L --fail -o $f $url
    }
    return $f
}

$ytdlp = Fetch "yt-dlp.exe" "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"

$ffzip = Fetch "ffmpeg.zip" "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
$ffdir = Join-Path $cache "ffmpeg"
if (-not (Test-Path "$ffdir\ffmpeg.exe")) {
    Write-Host "==> ffmpeg 추출" -ForegroundColor Cyan
    $tmp = Join-Path $cache "ffmpeg-tmp"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-Archive -Path $ffzip -DestinationPath $tmp -Force
    New-Item -ItemType Directory -Force $ffdir | Out-Null
    Copy-Item (Get-ChildItem $tmp -Recurse -Filter ffmpeg.exe | Select-Object -First 1).FullName "$ffdir\ffmpeg.exe"
    Remove-Item $tmp -Recurse -Force
}

$denozip = Fetch "deno.zip" "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip"
$denodir = Join-Path $cache "deno"
if (-not (Test-Path "$denodir\deno.exe")) {
    Write-Host "==> deno 추출" -ForegroundColor Cyan
    Expand-Archive -Path $denozip -DestinationPath $denodir -Force
}

Write-Host "==> 파일 모으기" -ForegroundColor Cyan
Copy-Item "build\Release\youcansingwell.exe" "$stage\YouCanSingWell.exe"
Copy-Item "build\Release\ycs_separate.exe" "$stage\ycs_separate.exe"
Copy-Item $ytdlp "$stage\bin\yt-dlp.exe"
Copy-Item "$ffdir\ffmpeg.exe" "$stage\bin\ffmpeg.exe"
Copy-Item "$denodir\deno.exe" "$stage\bin\deno.exe"
Copy-Item "LICENSE" "$stage\LICENSE.txt"
Copy-Item "README.md" "$stage\README.md"
Copy-Item "third_party\demucs.cpp\LICENSE" "$stage\LICENSE-demucs.cpp.txt"
@"
YouCanSingWell v$version
========================
YouCanSingWell.exe 를 실행하세요. 노래와 연습 데이터는 %LOCALAPPDATA%\YouCanSingWell 에 저장됩니다.
유튜브 노래를 처음 가져올 때 보컬 분리 모델(약 80MB) 을 한 번 받습니다.

동봉: ycs_separate.exe (보컬 분리, demucs.cpp), bin\ 의 yt-dlp · ffmpeg · deno (유튜브 다운로드용)
"@ | Out-File -Encoding utf8 "$stage\READ_ME_FIRST.txt"

$zip = Join-Path $dist "YouCanSingWell-v$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Write-Host "==> zip" -ForegroundColor Cyan
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

if (-not $SkipInstaller) {
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($iscc) {
        Write-Host "==> Inno Setup" -ForegroundColor Cyan
        & $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DOutDir=$dist" "installer\youcansingwell.iss"
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
    } else {
        Write-Host "Inno Setup 이 없어 설치 프로그램은 건너뜁니다 (winget install JRSoftware.InnoSetup)" -ForegroundColor Yellow
    }
}

Write-Host ""
Get-ChildItem $dist -File | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB,1)}} | Format-Table -AutoSize
