#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HostProject,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [switch]$AllowOtherProjects
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$hostFile = [IO.Path]::GetFullPath($HostProject)
if (!$hostFile.StartsWith((Join-Path $taskRoot 'build')+'\',[StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($hostFile) -ne 'AssetPreviewHost.uproject' -or !(Test-Path -LiteralPath $hostFile)) { throw 'Spatial fixtures require an isolated build host.' }
if ($AllowOtherProjects) { & (Join-Path $PSScriptRoot 'Assert-HostIsolation.ps1') -HostProject $hostFile }
elseif (@(Get-Process UnrealEditor,UnrealEditor-Cmd,LiveCodingConsole -ErrorAction SilentlyContinue).Count) { throw 'Close Unreal Editor/Live Coding or use verified isolated -AllowOtherProjects.' }
$output = Join-Path $taskRoot ('build\spatial-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$log = Join-Path $output 'Unreal.log'
$info = [Diagnostics.ProcessStartInfo]::new()
$info.FileName = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
foreach($argument in @($hostFile,'-RenderOffscreen','-unattended','-nosplash','-nosound','-NoLiveCoding',"-abslog=$log",'-RiderAssetPreviewPort=32190',
    '-ExecCmds=Automation RunTests RiderAssetPreview.Spatial','-TestExit=Automation Test Queue Empty',"-ReportExportPath=$output")) { $info.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::Start($info)
$process.BeginOutputReadLine(); $process.BeginErrorReadLine()
Write-Output "Started isolated map/widget tests PID $($process.Id)."
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    while (!$process.HasExited) {
        if ([DateTime]::UtcNow -gt $deadline) { throw "Spatial tests timed out. See $log" }
        Start-Sleep -Milliseconds 500
    }
    $content = Get-Content -Raw -LiteralPath $log
    if ($process.ExitCode -ne 0 -or $content -notmatch 'Test Completed\. Result=\{Success\}[^\r\n]*RiderAssetPreview\.Spatial\.IsolatedContent') {
        Get-Content -LiteralPath $log -Tail 45
        throw "Spatial automation failed (exit $($process.ExitCode)). See $log"
    }
    $evidence = Join-Path ([IO.Path]::GetDirectoryName($hostFile)) 'Saved\RiderAssetPreview\SpatialEvidence'
    foreach($name in @('map-before.json','map-after.json','widget-before.json','widget-after.json','map-before.png','map-after.png','widget-before.png','widget-after.png','fixture-paths.json')) {
        if (!(Test-Path -LiteralPath (Join-Path $evidence $name))) { throw "Missing spatial evidence: $name" }
        if ($name.EndsWith('.json')) {
            # Unreal's AutoDetect saves non-ASCII FString JSON as UTF-16. Keep
            # cross-language evidence consistently UTF-8 without changing values.
            [IO.File]::WriteAllText((Join-Path $output $name), [IO.File]::ReadAllText((Join-Path $evidence $name)), [Text.UTF8Encoding]::new($false))
        } else { Copy-Item -LiteralPath (Join-Path $evidence $name) -Destination $output }
    }
    Write-Output "PASS: real map/widget revision loads, rendered previews, active editor level preservation and skipped Blueprint PreConstruct. Evidence: $output"
} finally {
    if (!$process.HasExited) { $process.Kill(); $null = $process.WaitForExit(10000) }
    $process.Dispose()
}
