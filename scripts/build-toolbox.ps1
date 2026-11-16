[CmdletBinding()]
param(
    [string]$QtPrefix = 'E:\DevCaches\snow-toolbox-build\Qt',
    [string]$BuildDirectory = 'build/toolbox-release',
    [string]$OcrExecutable = '',
    [string]$CMakeExecutable = '',
    [int]$Parallelism = 2,
    [switch]$RuntimeTests,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
if (-not $CMakeExecutable) {
    $managedCMake = Join-Path $repo '.tools/vcpkg/downloads/tools/cmake-4.2.3-windows/cmake-4.2.3-windows-x86_64/bin/cmake.exe'
    $CMakeExecutable = if (Test-Path -LiteralPath $managedCMake) { $managedCMake } else { (Get-Command cmake -ErrorAction Stop).Source }
}
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
Add-SnowMsvcToolsToPath -Architecture x64 | Out-Null
$env:PATH = "$env:USERPROFILE\.cargo\bin;$QtPrefix\bin;$env:PATH"
$env:CARGO_BUILD_JOBS = [string]$Parallelism
$env:VCPKG_MAX_CONCURRENCY = [string]$Parallelism
if (-not $env:LIBCLANG_PATH) {
    $libclang = Join-Path $repo '.tools/vcpkg/downloads/tools/clang/clang-15.0.6/bin'
    if (Test-Path -LiteralPath "$libclang/libclang.dll") { $env:LIBCLANG_PATH = $libclang }
}
if ($env:LIBCLANG_PATH) { $env:PATH = "$env:LIBCLANG_PATH;$env:PATH" }
$python = (Get-Command python -ErrorAction Stop).Source
$ninja = Join-Path $env:VSINSTALLDIR 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (-not (Test-Path -LiteralPath $ninja)) { $ninja = (Get-Command ninja -ErrorAction Stop).Source }
& $CMakeExecutable -S . -B $BuildDirectory -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_PREFIX_PATH=$QtPrefix" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL `
    "-DPython3_EXECUTABLE=$python" `
    "-DCMAKE_TOOLCHAIN_FILE=$repo/.tools/vcpkg/scripts/buildsystems/vcpkg.cmake" `
    "-DVCPKG_INSTALLED_DIR=$repo/.tools/vcpkg/installed/dynamic" `
    -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_HOST_TRIPLET=x64-windows -DVCPKG_MANIFEST_FEATURES=snow-shot `
    "-DVCPKG_OVERLAY_PORTS=$repo/cmake/vcpkg-overlay-ports" `
    "-DVCPKG_OVERLAY_TRIPLETS=$repo/cmake/vcpkg-overlay-triplets" `
    -DSNOW_APPS_BUILD_TESTS=OFF -DSNOW_APPS_BUILD_BENCHMARKS=OFF `
    "-DSNOW_SHOT_BUILD_RUNTIME_TESTS=$($RuntimeTests.IsPresent)" `
    -DSNOW_APPS_BUILD_IMAGE_VIEWER=OFF -DSNOW_APPS_BUILD_SNOW_SHOT_MINI=OFF `
    -DSNOW_APPS_ENABLE_RELEASE_OPTIMIZATION=OFF -DSNOW_APPS_RELEASE_STATIC=OFF `
    -DSNOW_APPS_PACKAGE_SNOW_SHOT=OFF -DSNOW_IMAGE_PROFILE=redistributable `
    -DSNOW_SHOT_ENABLE_MCP=OFF -DSNOW_APPS_API_BASE_URL=http://127.0.0.1 `
    -DSNOW_APPS_FREE_TRANSLATION_API_BASE_URL=http://127.0.0.1 `
    "-DSNOW_TOOLBOX_OCR_EXECUTABLE=$OcrExecutable"
if ($LASTEXITCODE) { throw 'Snow Toolbox configuration failed' }
if (-not $ConfigureOnly) {
    & $CMakeExecutable --build $BuildDirectory --target snow_shot --parallel $Parallelism
    if ($LASTEXITCODE) { throw 'Snow Toolbox build failed' }
}
