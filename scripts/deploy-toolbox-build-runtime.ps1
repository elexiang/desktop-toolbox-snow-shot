[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/toolbox-release',
    [string]$QtPrefix = 'E:\DevCaches\snow-toolbox-build\Qt',
    [string]$PythonRuntime = 'E:\gitee_linya_projects\desktop-toolbox\runtime\python'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$buildPath = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $repo $BuildDirectory
}
$binary = Join-Path $buildPath 'snow_shot/SnowToolbox.exe'
$deploy = Join-Path $QtPrefix 'bin/windeployqt.exe'
foreach ($required in @($binary, $deploy, (Join-Path $PythonRuntime 'python.exe'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing runtime deployment input: $required"
    }
}
$binaryDirectory = Split-Path $binary -Parent
& $deploy --release --no-translations --no-opengl-sw --no-compiler-runtime $binary
if ($LASTEXITCODE) { throw 'Build-tree Qt runtime deployment failed' }
Copy-Item -LiteralPath (Join-Path $QtPrefix 'plugins/platforms/qoffscreen.dll') `
    -Destination (Join-Path $binaryDirectory 'platforms') -Force
Set-Content -LiteralPath (Join-Path $binaryDirectory 'qt.conf') -Encoding ascii `
    -Value @('[Paths]', 'Prefix=.', 'Plugins=.', 'Translations=translations')
$runtimeDirectory = Join-Path $binaryDirectory 'runtime'
New-Item -ItemType Directory -Force -Path $runtimeDirectory | Out-Null
Copy-Item -LiteralPath $PythonRuntime -Destination $runtimeDirectory -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repo 'scripts/toolbox_codex_context.py') `
    -Destination (Join-Path $runtimeDirectory 'codex_context.py') -Force
Write-Output "Build-tree Qt runtime deployed: $binaryDirectory"
