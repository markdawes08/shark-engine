# Internal local deployment verifier; this script is not a Runtime dependency.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{32}$')][string]$Device,
    [switch]$Cube,
    [switch]$Scene,
    [switch]$DefaultStartup,
    [switch]$Inspect,
    [string]$WorkingDirectory,
    [string]$CaptureDirectory
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
function Assert-OrdinaryPath([string]$Value) {
    if ([string]::IsNullOrEmpty($Value) -or $Value.Length -gt 4096 -or $Value -match '["\x00-\x1f]') {
        throw 'Package verifier paths must be bounded ordinary path text'
    }
    $taskPathPart = [IO.Path]::GetFullPath($Value)
    while ($taskPathPart) {
        if (Test-Path -LiteralPath $taskPathPart) {
            $taskPathItem = Get-Item -LiteralPath $taskPathPart -Force
            if (($taskPathItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Package verifier paths must not contain reparse points'
            }
        }
        $taskNextPart = [IO.Path]::GetDirectoryName($taskPathPart)
        if ($taskNextPart -eq $taskPathPart) { break }
        $taskPathPart = $taskNextPart
    }
}
function Test-InPackage([string]$Value, [string]$Root) {
    return $Value.Equals($Root, [StringComparison]::OrdinalIgnoreCase) -or
        $Value.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}
function Quote-Argument([string]$Value) {
    if ($Value -match '["\x00-\x1f]' -or $Value.EndsWith('\')) {
        throw 'Unsupported quoted argument in package verifier'
    }
    return '"' + $Value + '"'
}
$taskModeCount = [int]$Cube.IsPresent + [int]$Scene.IsPresent + [int]$DefaultStartup.IsPresent + [int]$Inspect.IsPresent
if ($taskModeCount -gt 1) { throw 'Package verifier modes are mutually exclusive' }
$taskMode = 'graphics'
if ($Cube) { $taskMode = 'cube' }
if ($Scene) { $taskMode = 'scene' }
if ($DefaultStartup) { $taskMode = 'default' }
if ($Inspect) { $taskMode = 'inspect' }
Assert-OrdinaryPath $Package
$taskPackage = (Resolve-Path -LiteralPath $Package).Path
if (-not (Test-Path -LiteralPath $taskPackage -PathType Container)) { throw 'Package must be an existing directory' }
$taskWorkingDirectory = $taskPackage
if ($Scene -or $DefaultStartup -or $Inspect) {
    Assert-OrdinaryPath $WorkingDirectory
    $taskWorkingDirectory = (Resolve-Path -LiteralPath $WorkingDirectory).Path
    if (-not (Test-Path -LiteralPath $taskWorkingDirectory -PathType Container) -or
        (Test-InPackage $taskWorkingDirectory $taskPackage)) {
        throw 'Scene/default/inspection working directory must exist outside the package'
    }
} elseif ($WorkingDirectory) {
    throw 'WorkingDirectory applies only to scene, default startup or inspection'
}
$taskCaptureDirectory = $null
if ($Scene) {
    Assert-OrdinaryPath $CaptureDirectory
    $taskCaptureDirectory = [IO.Path]::GetFullPath($CaptureDirectory).TrimEnd('\')
    $taskCaptureParent = [IO.Path]::GetDirectoryName($taskCaptureDirectory)
    if ((Test-Path -LiteralPath $taskCaptureDirectory) -or
        -not (Test-Path -LiteralPath $taskCaptureParent -PathType Container) -or
        (Test-InPackage $taskCaptureDirectory $taskPackage)) {
        throw 'Scene capture must be a fresh directory outside the package with an existing parent'
    }
} elseif ($CaptureDirectory) {
    throw 'CaptureDirectory applies only to scene mode'
}
$taskSystem = [Environment]::GetEnvironmentVariable('SystemRoot')
$taskInfo = [Diagnostics.ProcessStartInfo]::new()
$taskInfo.FileName = Join-Path $taskPackage 'bin\Runtime.exe'
$taskArguments = @('--graphics', '--graphics-ms', '1000', '--device', $Device, '--validation', 'off')
if ($Cube) { $taskArguments[0] = '--cube' }
if ($Scene) {
    $taskArguments = @('--package', (Join-Path $taskPackage 'bin\startup.sharkstart'),
        '--extent', '160x120', '--frames', '2', '--device', $Device, '--validation', 'off',
        '--capture-directory', $taskCaptureDirectory)
}
if ($Inspect) { $taskArguments = @('--check-package', (Join-Path $taskPackage 'bin\startup.sharkstart')) }
if ($DefaultStartup) { $taskArguments = @() }
$taskInfo.Arguments = ($taskArguments | ForEach-Object { Quote-Argument $_ }) -join ' '
$taskInfo.WorkingDirectory = $taskWorkingDirectory
$taskInfo.UseShellExecute = $false
$taskInfo.RedirectStandardOutput = $true
$taskInfo.RedirectStandardError = $true
$taskInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
$taskInfo.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
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
$taskWindowObserved = $false
$taskWindowObservedAt = 0.0
$taskWindowProcess = 0
$taskCloseRequested = $false
$taskDefaultObservationSeconds = 1.0
if ($DefaultStartup) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SharkPackageWindow {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr window);
}
'@
}
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
        if ($DefaultStartup -and -not $taskCloseRequested -and -not $taskProcess.HasExited) {
            $taskWindow = [IntPtr]::Zero
            try { $taskWindow = $taskProcess.MainWindowHandle } catch { $taskPollErrors += 1 }
            [uint32]$taskOwner = 0
            if ($taskWindow -ne [IntPtr]::Zero -and
                [SharkPackageWindow]::GetWindowThreadProcessId($taskWindow, [ref]$taskOwner) -ne 0 -and
                $taskOwner -eq $taskProcess.Id -and [SharkPackageWindow]::IsWindowVisible($taskWindow)) {
                if (-not $taskWindowObserved) {
                    $taskWindowObserved = $true
                    $taskWindowObservedAt = $taskTimer.Elapsed.TotalSeconds
                    $taskWindowProcess = $taskOwner
                }
                # Bounded interactive workload: permit one second of visible
                # native presentation, then request the ordinary window close.
                # The caller must also verify positive completed frame counts.
                if ($taskTimer.Elapsed.TotalSeconds - $taskWindowObservedAt -ge $taskDefaultObservationSeconds) {
                    if (-not $taskProcess.CloseMainWindow()) { throw 'Could not request normal Runtime window close' }
                    $taskCloseRequested = $true
                }
            }
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
    if ([Text.Encoding]::UTF8.GetByteCount($taskStdout.Result) -gt 1048576 -or
        [Text.Encoding]::UTF8.GetByteCount($taskStderr.Result) -gt 1048576) {
        throw 'Runtime verifier output exceeded its report byte bound'
    }
    $taskDefaultStartup = $null
    if ($DefaultStartup) {
        $taskDefaultStartup = [ordered]@{
            window_observed = $taskWindowObserved
            window_process_id = $taskWindowProcess
            process_id = $taskProcess.Id
            observation_seconds = $taskDefaultObservationSeconds
            close_requested = $taskCloseRequested
        }
    }
    [ordered]@{
        mode = $taskMode
        working_directory = $taskWorkingDirectory
        capture_directory = $taskCaptureDirectory
        arguments = $taskInfo.Arguments
        command = @($taskInfo.FileName) + $taskArguments
        default_startup = $taskDefaultStartup
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
