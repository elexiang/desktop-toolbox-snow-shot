[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/toolbox-release',
    [string]$QtPrefix = 'E:\DevCaches\snow-toolbox-build\Qt',
    [string]$UpstreamRuntime = 'E:\DevCaches\snow-toolbox-build\upstream-runtime',
    [string]$TranslationRuntime = 'E:\gitee_linya_projects\desktop-toolbox\runtime',
    [string]$OutputDirectory = 'artifacts/SnowToolbox-0.3.0-win-x64'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
$binary = Join-Path $repo "$BuildDirectory/snow_shot/SnowToolbox.exe"
$updater = Join-Path $repo "$BuildDirectory/snow_shot/snow-shot-updater.exe"
foreach ($required in @($binary, $updater, "$QtPrefix/bin/windeployqt.exe", "$UpstreamRuntime/bin/assets/ocr/asset-manifest.json", "$UpstreamRuntime/bin/assets/qrcode/detect.caffemodel", "$TranslationRuntime/python/python.exe", "$TranslationRuntime/models/en_zh", "$TranslationRuntime/models/zh_en")) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing package input: $required" }
}
# Verify the upstream OCR files against the verified release archive's file inventory.
$inventory = Get-Content -LiteralPath "$UpstreamRuntime/snow-shot-installation.json" -Raw | ConvertFrom-Json
foreach ($item in $inventory.files) {
    if ($item.path -notlike 'bin/assets/ocr/*' -and $item.path -notlike 'bin/assets/qrcode/*') { continue }
    $file = Join-Path $UpstreamRuntime $item.path
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256) {
        throw "Upstream OCR hash mismatch: $($item.path)"
    }
}
New-Item -ItemType Directory -Force -Path $output,"$output/runtime","$output/licenses","$output/assets" | Out-Null
Copy-Item -LiteralPath $binary -Destination "$output/SnowToolbox.exe" -Force
Copy-Item -LiteralPath $updater -Destination "$output/snow-shot-updater.exe" -Force
Get-ChildItem -LiteralPath (Join-Path $repo $BuildDirectory) -Recurse -Filter '*.dll' -File |
    Where-Object { $_.FullName -notmatch '[\\/]cargo[\\/]' } |
    Copy-Item -Destination $output -Force
$env:PATH = "$QtPrefix/bin;$env:PATH"
& "$QtPrefix/bin/windeployqt.exe" --release --no-translations --no-opengl-sw --no-compiler-runtime "$output/SnowToolbox.exe"
if ($LASTEXITCODE) { throw 'Qt runtime deployment failed' }
# The upstream updater's isolated health probe explicitly uses the offscreen platform.
Copy-Item -LiteralPath "$QtPrefix/plugins/platforms/qoffscreen.dll" -Destination "$output/platforms" -Force
Copy-Item -Path '.tools/vcpkg/installed/dynamic/x64-windows/bin/*.dll' -Destination $output -Force
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
Add-SnowMsvcToolsToPath -Architecture x64 | Out-Null
$crt = Get-ChildItem -LiteralPath "$env:VSINSTALLDIR/VC/Redist/MSVC" -Directory |
    Where-Object { $_.Name -match '^\d+\.' } | Sort-Object Name -Descending | Select-Object -First 1
Copy-Item -Path "$($crt.FullName)/x64/Microsoft.VC145.CRT/*.dll" -Destination $output -Force
Copy-Item -LiteralPath "$UpstreamRuntime/bin/assets/ocr" -Destination "$output/assets" -Recurse -Force
Copy-Item -LiteralPath "$UpstreamRuntime/bin/assets/qrcode" -Destination "$output/assets" -Recurse -Force
Copy-Item -LiteralPath "$UpstreamRuntime/bin/crashpad_handler.exe" -Destination $output -Force
if (Test-Path -LiteralPath "$UpstreamRuntime/bin/audios") {
    Copy-Item -LiteralPath "$UpstreamRuntime/bin/audios" -Destination $output -Recurse -Force
}
foreach ($directory in @('python','models')) {
    Copy-Item -LiteralPath "$TranslationRuntime/$directory" -Destination "$output/runtime" -Recurse -Force
}
Copy-Item -LiteralPath scripts/toolbox_translate_worker.py -Destination "$output/runtime/translate_worker.py" -Force
Copy-Item -LiteralPath scripts/toolbox_codex_context.py -Destination "$output/runtime/codex_context.py" -Force
Copy-Item -LiteralPath scripts/toolbox-runtime-requirements.txt -Destination "$output/runtime/requirements.txt" -Force
New-Item -ItemType Directory -Force -Path "$output/licenses/upstream-ocr" | Out-Null
Copy-Item -LiteralPath "$UpstreamRuntime/share/snow-shot/licenses/LICENSES.txt" -Destination "$output/licenses/upstream-ocr/LICENSES.txt" -Force
Copy-Item -LiteralPath snow_shot/LICENSE -Destination "$output/licenses/GPL-3.0.txt" -Force
Copy-Item -LiteralPath LICENSE.md -Destination "$output/licenses/Snow-Apps-Licensing.md" -Force
Copy-Item -LiteralPath snow_shot/THIRD_PARTY_NOTICES.md -Destination "$output/THIRD_PARTY_NOTICES.md" -Force
Copy-Item -LiteralPath README_TOOLBOX.md -Destination "$output/使用说明.md" -Force
Set-Content -LiteralPath "$output/qt.conf" -Encoding ascii -Value @('[Paths]','Prefix=.','Plugins=.','Translations=translations')
& (Join-Path $PSScriptRoot 'collect-third-party-licenses.ps1') `
    -Destination "$output/licenses/third-party" -AllowedRoot $output `
    -VcpkgPrefix (Join-Path $repo '.tools/vcpkg/installed/dynamic/x64-windows') `
    -QtPrefix $QtPrefix -CargoManifest @((Join-Path $repo 'snow_rust_ffi/Cargo.toml'), (Join-Path $repo 'snow_shot/rust/snow-shot-updater/Cargo.toml')) `
    -CargoOptions @{ (Join-Path $repo 'snow_rust_ffi/Cargo.toml') = @('--features','selected-text,language-detection') } `
    -AntDesignNotice (Join-Path $repo 'ant_design_qt/THIRD_PARTY_NOTICES.md') `
    -FallbackLicenseDirectory (Join-Path $repo 'licenses')
if ($LASTEXITCODE) { throw 'License collection failed' }
$entries = Get-ChildItem -LiteralPath $output -Recurse -File | Where-Object Name -ne 'sha256.json' | ForEach-Object {
    [ordered]@{path=[IO.Path]::GetRelativePath($output,$_.FullName).Replace('\','/'); size=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$entries | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$output/sha256.json" -Encoding utf8
Write-Output "Portable folder: $output"
