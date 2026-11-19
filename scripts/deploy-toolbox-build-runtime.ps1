[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/toolbox-release',
    [ValidatePattern('^[A-Za-z0-9_-]+\.exe$')][string]$ExecutableName = 'SnowToolbox.exe',
    [string]$QtPrefix = 'E:\DevCaches\snow-toolbox-build\Qt',
    [string]$PythonRuntime = (Join-Path (Split-Path $PSScriptRoot -Parent) '.tools/toolbox-runtime/python')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$buildPath = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $repo $BuildDirectory
}
$binary = Join-Path $buildPath "snow_shot/$ExecutableName"
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
$modelsDirectory = Join-Path (Split-Path $PythonRuntime -Parent) 'models'
if (Test-Path -LiteralPath $modelsDirectory -PathType Container) {
    Copy-Item -LiteralPath $modelsDirectory -Destination $runtimeDirectory -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repo 'scripts/toolbox_translate_worker.py') `
        -Destination (Join-Path $runtimeDirectory 'translate_worker.py') -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'scripts/toolbox_codex_context.py') `
    -Destination (Join-Path $runtimeDirectory 'codex_context.py') -Force
Write-Output "Build-tree Qt runtime deployed: $binaryDirectory"
