[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [string]$HostProject,
    [ValidateRange(1, 3)][int]$MaxParallelActions = 1
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
# Live Coding and editor-loaded DLLs make external builds unreliable. Never close user sessions.
$activeEditors = @(Get-Process -Name UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue)
$activeLiveCoding = @(Get-Process -Name LiveCodingConsole -ErrorAction SilentlyContinue)
if ($activeEditors.Count -gt 0 -or $activeLiveCoding.Count -gt 0) {
    Write-Output 'BLOCKED (20): Close Unreal Editor and Live Coding manually, then run this script again. No build was started.'
    exit 20
}
$buildCommand = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
if (!(Test-Path -LiteralPath $buildCommand)) { throw "Unreal Build.bat not found: $EngineRoot" }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($HostProject) {
    $projectFile = [IO.Path]::GetFullPath($HostProject)
    $allowedRoot = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')) + [IO.Path]::DirectorySeparatorChar
    if (!$projectFile.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $projectFile)) {
        throw 'A reused host must be an existing project beneath this repository build directory.'
    }
    $hostRoot = [IO.Path]::GetDirectoryName($projectFile)
} else {
    $hostRoot = Join-Path $taskRoot ('build\unreal-host-' + $stamp + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $projectFile = Join-Path $hostRoot 'AssetPreviewHost.uproject'
}
$hostPlugins = Join-Path $hostRoot 'Plugins'
New-Item -ItemType Directory -Path $hostPlugins -Force | Out-Null
$pluginSource = Join-Path $taskRoot 'unreal\RiderAssetPreview'
$pluginCopy = Join-Path $hostPlugins 'RiderAssetPreview'
New-Item -ItemType Directory -Path $pluginCopy -Force | Out-Null
Get-ChildItem -LiteralPath $pluginSource -Force | Copy-Item -Destination $pluginCopy -Recurse -Force
$projectData = @{
    FileVersion = 3
    EngineAssociation = '5.8'
    Category = 'Development'
    Description = 'Isolated build host for RiderAssetPreview'
    Plugins = @(@{ Name = 'RiderAssetPreview'; Enabled = $true })
} | ConvertTo-Json -Depth 5
if (!$HostProject) { [IO.File]::WriteAllText($projectFile, $projectData, [Text.UTF8Encoding]::new($false)) }
$pluginFile = Join-Path $pluginCopy 'RiderAssetPreview.uplugin'
& $buildCommand 'UnrealEditor' 'Win64' 'Development' "-Project=$projectFile" "-Plugin=$pluginFile" '-NoEngineChanges' '-NoHotReloadFromIDE' '-NoUBA' "-MaxParallelActions=$MaxParallelActions"
if ($LASTEXITCODE -ne 0) { throw "Unreal plugin build failed (exit $LASTEXITCODE). See the UBT log; no automatic retry was made." }
$packageRoot = Join-Path $taskRoot ('dist\RiderAssetPreview-UE5.8-' + $stamp)
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
foreach ($entry in @('RiderAssetPreview.uplugin', 'Source', 'Binaries')) {
    $source = Join-Path $pluginCopy $entry
    if (!(Test-Path -LiteralPath $source)) { throw "Expected build output missing: $source" }
    Copy-Item -LiteralPath $source -Destination $packageRoot -Recurse
}
Write-Output "Built plugin: $packageRoot"
Write-Output "Host project: $projectFile"
Write-Output 'Copy this directory to <YourProject>\Plugins\RiderAssetPreview and enable the plugin in Unreal Editor.'
