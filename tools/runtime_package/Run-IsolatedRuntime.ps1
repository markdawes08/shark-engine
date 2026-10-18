# Internal local deployment verifier; this script is not a Runtime dependency.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{32}$')][string]$Device
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$taskPackage = (Resolve-Path -LiteralPath $Package).Path
$taskSystem = [Environment]::GetEnvironmentVariable('SystemRoot')
$taskInfo = [Diagnostics.ProcessStartInfo]::new()
$taskInfo.FileName = Join-Path $taskPackage 'bin\Runtime.exe'
$taskInfo.Arguments = "--graphics --graphics-ms 1000 --device $Device --validation off"
$taskInfo.WorkingDirectory = $taskPackage
$taskInfo.UseShellExecute = $false
$taskInfo.RedirectStandardOutput = $true
$taskInfo.RedirectStandardError = $true
$taskInfo.EnvironmentVariables.Clear()
$taskInfo.EnvironmentVariables['SystemRoot'] = $taskSystem
$taskInfo.EnvironmentVariables['WINDIR'] = $taskSystem
$taskInfo.EnvironmentVariables['PATH'] = Join-Path $taskSystem 'System32'
$taskInfo.EnvironmentVariables['TEMP'] = [IO.Path]::GetTempPath()
$taskInfo.EnvironmentVariables['TMP'] = [IO.Path]::GetTempPath()
# Disable implicit layers without exposing development layer/ICD overrides.
$taskInfo.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
$taskProcess = [Diagnostics.Process]::new()
$taskProcess.StartInfo = $taskInfo
$taskModules = @{}
$taskPollErrors = 0
$taskTimedOut = $false
$taskStarted = $false
$taskTimer = [Diagnostics.Stopwatch]::StartNew()
try {
    if (-not $taskProcess.Start()) { throw 'Could not start Runtime' }
    $taskStarted = $true
    $taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
    $taskStderr = $taskProcess.StandardError.ReadToEndAsync()
    while (-not $taskProcess.HasExited) {
        try {
            $taskProcess.Refresh()
            foreach ($taskModule in $taskProcess.Modules) {
                $taskModules[$taskModule.FileName] = $true
            }
        } catch {
            # Startup/exit races are retained; the verifier requires the complete
            # known Runtime/loader/CRT module set from successful observations.
            $taskPollErrors += 1
        }
        if ($taskTimer.Elapsed.TotalSeconds -ge 15) {
            $taskTimedOut = $true
            $taskProcess.Kill()
            break
        }
        [void]$taskProcess.WaitForExit(10)
    }
    if (-not $taskProcess.WaitForExit(1000)) { throw 'Runtime exceeded cleanup deadline' }
    if (-not $taskStdout.Wait(1000) -or -not $taskStderr.Wait(1000)) {
        throw 'Runtime output exceeded cleanup deadline'
    }
    [ordered]@{
        exit_code = $taskProcess.ExitCode
        timed_out = $taskTimedOut
        elapsed_seconds = $taskTimer.Elapsed.TotalSeconds
        stdout = $taskStdout.Result
        stderr = $taskStderr.Result
        modules = @($taskModules.Keys | Sort-Object)
        module_poll_errors = $taskPollErrors
        environment = [ordered]@{ PATH = $taskInfo.EnvironmentVariables['PATH']; SystemRoot = $taskSystem; VK_LOADER_LAYERS_DISABLE = '~implicit~'; inherited_development_overrides = $false }
    } | ConvertTo-Json -Depth 8
} finally {
    if ($taskStarted -and -not $taskProcess.HasExited) { $taskProcess.Kill() }
    $taskProcess.Dispose()
}
