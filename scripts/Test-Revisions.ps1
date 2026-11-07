#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HostProject,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [switch]$AllowOtherProjects
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$hostFile=[IO.Path]::GetFullPath($HostProject)
$allowed=[IO.Path]::GetFullPath((Join-Path $taskRoot 'build')) + '\'
if (!$hostFile.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $hostFile -PathType Leaf) -or
    [IO.Path]::GetFileName($hostFile) -ne 'AssetPreviewHost.uproject') { throw 'Revision fixtures require an isolated AssetPreviewHost inside this checkout build directory.' }
if (@(Get-Process -Name UnrealEditor,UnrealEditor-Cmd,LiveCodingConsole -ErrorAction SilentlyContinue).Count -and !$AllowOtherProjects) {
    throw 'Close Unreal Editor and Live Coding first. This test never closes existing sessions.'
}
if($AllowOtherProjects){& (Join-Path $PSScriptRoot 'Assert-HostIsolation.ps1') -HostProject $hostFile}
$output=Join-Path $taskRoot ('build\revisions-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$log=Join-Path $output 'Unreal.log'
$info=[Diagnostics.ProcessStartInfo]::new()
$info.FileName=Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$info.UseShellExecute=$false
$info.CreateNoWindow=$true
$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
foreach ($argument in @($hostFile,'-RenderOffscreen','-unattended','-nosplash','-nosound','-NoLiveCoding',"-abslog=$log",
    '-RiderAssetPreviewPort=32190','-ExecCmds=Automation RunTests RiderAssetPreview.Revisions','-TestExit=Automation Test Queue Empty',"-ReportExportPath=$output")) { $info.ArgumentList.Add($argument) }
$process=[Diagnostics.Process]::Start($info)
$process.BeginOutputReadLine();$process.BeginErrorReadLine()
Write-Output "Started isolated Unreal revision tests (PID $($process.Id))."
try {
    $deadline=[DateTime]::UtcNow.AddMinutes(5)
    while (!$process.HasExited) {
        if ([DateTime]::UtcNow -gt $deadline) { throw "Revision tests timed out. See $log" }
        Start-Sleep -Milliseconds 500
    }
    $content=Get-Content -Raw -LiteralPath $log
    if ($process.ExitCode -ne 0 -or $content -notmatch 'Test Completed\. Result=\{Success\}[^\r\n]*RiderAssetPreview\.Revisions\.IsolatedBlueprint') {
        Get-Content -LiteralPath $log -Tail 45
        throw "Unreal revision tests failed (exit $($process.ExitCode)). See $log"
    }
    $evidence=Join-Path ([IO.Path]::GetDirectoryName($hostFile)) 'Saved\RiderAssetPreview\TestEvidence'
    foreach ($name in @('before.json','after.json','before.png','after.png','fixture-paths.json')) {
        if (!(Test-Path -LiteralPath (Join-Path $evidence $name))) { throw "Missing revision test evidence: $name" }
        Copy-Item -LiteralPath (Join-Path $evidence $name) -Destination $output
    }
    Write-Output "PASS real Unreal revision isolation, before/after settings, added components, Blueprint pin defaults, shared-camera PNGs and revision package cleanup. Evidence: $output"
} finally {
    if (!$process.HasExited) { $process.Kill();$null=$process.WaitForExit(10000) }
    $process.Dispose()
}
