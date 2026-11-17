[CmdletBinding()]
param([string]$Executable = 'build/toolbox-release/snow_shot/SnowToolbox.exe')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$binary = if ([IO.Path]::IsPathRooted($Executable)) {
    $Executable
} else {
    Join-Path $repo $Executable
}
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $binary
# An invalid ID exits before privileges or system cleanup are attempted.
$start.Arguments = '--toolbox-memory-helper invalid'
$start.WorkingDirectory = $env:SystemRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
foreach ($name in @('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QTDIR')) {
    $start.Environment.Remove($name) | Out-Null
}
$process = [Diagnostics.Process]::Start($start)
try {
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw 'Isolated memory helper startup timed out'
    }
    if ($process.ExitCode -ne 64) {
        throw "Isolated memory helper failed to reach request validation: $($process.ExitCode)"
    }
} finally {
    $process.Dispose()
}
Write-Output 'PASS: memory helper starts without development PATH and rejects invalid input (64)'
$runtime = Join-Path (Split-Path $binary -Parent) 'runtime'
$testRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('snow-context-' + [guid]::NewGuid())))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $config = Join-Path $testRoot 'config.toml'
    $original = "# runtime test`nmodel = `"test-model`"`n"
    [IO.File]::WriteAllText($config, $original, [Text.UTF8Encoding]::new($false))
    $revision = ''
    foreach ($action in @('read', 'on', 'off')) {
        $contextStart = [Diagnostics.ProcessStartInfo]::new()
        $contextStart.FileName = Join-Path $runtime 'python/python.exe'
        $contextStart.ArgumentList.Add((Join-Path $runtime 'codex_context.py'))
        $contextStart.ArgumentList.Add($action)
        if ($action -ne 'read') { $contextStart.ArgumentList.Add($revision) }
        $contextStart.UseShellExecute = $false
        $contextStart.CreateNoWindow = $true
        $contextStart.RedirectStandardOutput = $true
        $contextStart.Environment['CODEX_HOME'] = $testRoot
        $contextStart.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
        $context = [Diagnostics.Process]::Start($contextStart)
        try {
            if (-not $context.WaitForExit(10000)) {
                $context.Kill()
                throw "Context runtime timed out: $action"
            }
            $result = $context.StandardOutput.ReadToEnd() | ConvertFrom-Json
            if ($context.ExitCode -ne 0 -or $result.error -or -not $result.revision) {
                throw "Context runtime failed: $action"
            }
            $expected = if ($action -eq 'on') { '1m' } else { 'default' }
            if ($result.state -ne $expected) { throw "Unexpected context state: $action" }
            $revision = $result.revision
        } finally {
            $context.Dispose()
        }
    }
    if ([IO.File]::ReadAllText($config) -cne $original) { throw 'Context round trip changed unrelated content' }
    Write-Output 'PASS: bundled context runtime reads, enables and disables in an isolated profile'
} finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $testRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a context test profile outside the temporary directory'
    }
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
