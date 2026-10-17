# Run a Shark command in a native x64 MSVC environment without changing the
# caller's environment or requiring a particular Visual Studio installation path.
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $CommandArguments
)

$ErrorActionPreference = 'Stop'
if (-not $CommandArguments -or $CommandArguments.Count -eq 0) {
    throw 'Usage: powershell -File tools/windows_dev.ps1 python tools/ci.py --preset windows-debug'
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio with Desktop development with C++.'
}
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
    throw 'No Visual Studio installation with native x64 C++ tools was found.'
}
$developerModule = Join-Path $installation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll'
Import-Module $developerModule
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$localTools = Join-Path $repositoryRoot '.tools/venv/Scripts'
if (Test-Path -LiteralPath $localTools) {
    $env:VIRTUAL_ENV = Join-Path $repositoryRoot '.tools/venv'
    $env:PATH = "$localTools;$env:PATH"
}
Set-Location -LiteralPath $repositoryRoot
$commandName = $CommandArguments[0]
$remainingArguments = @()
if ($CommandArguments.Count -gt 1) {
    $remainingArguments = $CommandArguments[1..($CommandArguments.Count - 1)]
}
& $commandName @remainingArguments
exit $LASTEXITCODE
