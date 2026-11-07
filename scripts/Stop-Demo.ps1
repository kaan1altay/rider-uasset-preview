[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stateFile = Join-Path $taskRoot 'build\demo-state.json'
if (!(Test-Path -LiteralPath $stateFile)) { Write-Output 'No saved demo host.'; return }
$state = Get-Content -Raw -LiteralPath $stateFile | ConvertFrom-Json
$project = [IO.Path]::GetFullPath([string]$state.project)
$allowed = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (!$project.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) { throw 'The saved host is outside the isolated build directory.' }
$process = Get-CimInstance Win32_Process -Filter "ProcessId=$([int]$state.hostPid)"
if (!$process) { Write-Output 'Demo Unreal host is already closed.'; return }
$running = Get-Process -Id ([int]$state.hostPid)
$expectedStart = if ($state.hostStarted -is [DateTime]) {
    $state.hostStarted.ToUniversalTime()
} else {
    [DateTimeOffset]::Parse([string]$state.hostStarted).UtcDateTime
}
if ($process.Name -ne 'UnrealEditor-Cmd.exe' -or !$process.CommandLine.Contains($project) -or
    [Math]::Abs(($running.StartTime.ToUniversalTime() - $expectedStart).TotalSeconds) -gt 1) {
    throw 'Process identity no longer matches the saved demo. No process was stopped.'
}
Stop-Process -Id $running.Id -Force
Wait-Process -Id $running.Id -Timeout 15 -ErrorAction SilentlyContinue
if (Get-Process -Id $running.Id -ErrorAction SilentlyContinue) { throw 'The isolated demo host is still exiting.' }
Write-Output "Stopped only the isolated Unreal demo host PID $($running.Id). Close the RiderUi test window separately."
