[CmdletBinding()]
param(
    [string]$OutputPath,
    [string[]]$SteamRoot = @(),
    [string[]]$GameDirectory = @()
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputPath) {
    $OutputPath = Join-Path $projectRoot ('local/inputs-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
}
$scriptArgs = @((Join-Path $PSScriptRoot 'steam_inputs.py'), '--output', $OutputPath)
foreach ($root in $SteamRoot) { $scriptArgs += @('--steam-root', $root) }
foreach ($directory in $GameDirectory) { $scriptArgs += @('--game-dir', $directory) }
if (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 @scriptArgs
} elseif (Get-Command python3 -ErrorAction SilentlyContinue) {
    & python3 @scriptArgs
} else {
    throw 'An existing Python 3 installation is required. No dependency is downloaded automatically.'
}
if ($LASTEXITCODE -ne 0) { throw "Input inspection failed with exit code $LASTEXITCODE; review the preserved report." }
