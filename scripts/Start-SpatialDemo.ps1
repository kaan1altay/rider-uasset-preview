#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HostProject,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [string]$RiderHome = "$env:LOCALAPPDATA\Programs\Rider"
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project = [IO.Path]::GetFullPath($HostProject)
if (!$project.StartsWith((Join-Path $taskRoot 'build') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($project) -ne 'AssetPreviewHost.uproject' -or !(Test-Path -LiteralPath $project)) {
    throw 'Spatial demos require an isolated AssetPreviewHost inside this checkout.'
}
& (Join-Path $PSScriptRoot 'Assert-HostIsolation.ps1') -HostProject $project
$hostRoot = [IO.Path]::GetDirectoryName($project)
$fixture = Get-Content -Raw -LiteralPath (Join-Path $hostRoot 'Saved\RiderAssetPreview\SpatialEvidence\fixture-paths.json') | ConvertFrom-Json
$paths = @()
foreach ($kind in @('map','widget')) {
    $original = [IO.Path]::GetFullPath([string]$fixture.$kind)
    $before = [IO.Path]::GetFullPath([string]$fixture.($kind + 'Before'))
    $after = [IO.Path]::GetFullPath([string]$fixture.($kind + 'After'))
    if (!$original.StartsWith((Join-Path $hostRoot 'Content\RiderSpatialFixtures') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        !$before.StartsWith((Join-Path $hostRoot 'Saved\RiderAssetPreview\Diff') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        !$after.StartsWith((Join-Path $hostRoot 'Saved\RiderAssetPreview\Diff') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'All fixture paths must belong to this disposable host.'
    }
    if ((Get-FileHash -LiteralPath $original).Hash -ne (Get-FileHash -LiteralPath $after).Hash) {
        throw 'The working fixture changed since the test. No files were overwritten.'
    }
    $paths += @{ original=$original; before=$before; after=$after; relative=[IO.Path]::GetRelativePath($hostRoot,$original).Replace('\','/') }
}
$marker = Join-Path $hostRoot '.git\rider-spatial-fixture.json'
if (Test-Path -LiteralPath (Join-Path $hostRoot '.git')) {
    if (!(Test-Path -LiteralPath $marker) -and !(Test-Path -LiteralPath (Join-Path $hostRoot '.git\rider-review-fixture'))) {
        throw 'Refusing an unrecognized host Git repository.'
    }
    $remote = & git -C $hostRoot remote
    if ($LASTEXITCODE -or $remote) { throw 'The disposable fixture repository must have no remote.' }
} else {
    & git -C $hostRoot init --initial-branch=review
    if ($LASTEXITCODE) { throw 'Could not initialize fixture Git repository.' }
    '*' | Set-Content -LiteralPath (Join-Path $hostRoot '.gitignore') -Encoding utf8
}
if (Test-Path -LiteralPath $marker) {
    $saved = Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json
    if (($saved.original -join '|') -ne ($paths.original -join '|')) { throw 'Use a fresh host for different generated spatial fixtures.' }
} else {
    $staged = & git -C $hostRoot diff --cached --name-only
    if ($LASTEXITCODE -or $staged) { throw 'The fixture Git index must be empty before preparing the demo.' }
    $emptyHooks = Join-Path $hostRoot 'Saved\ReviewEmptyHooks'
    New-Item -ItemType Directory -Path $emptyHooks -Force | Out-Null
    try {
        foreach ($path in $paths) {
            Copy-Item -LiteralPath $path.before -Destination $path.original -Force
            & git -C $hostRoot add -f -- $path.relative
            if ($LASTEXITCODE) { throw 'Could not stage the spatial fixture.' }
        }
        & git -C $hostRoot -c "core.hooksPath=$emptyHooks" -c user.name='Rider Asset Preview Tests' -c user.email='tests@example.invalid' commit -m 'Before: map and widget review fixtures'
        if ($LASTEXITCODE) { throw 'Could not commit the spatial fixtures.' }
        $paths | ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding utf8
    } finally {
        foreach ($path in $paths) { Copy-Item -LiteralPath $path.after -Destination $path.original -Force }
    }
}
$idea = Join-Path $hostRoot 'Saved\RiderUi\.idea'
New-Item -ItemType Directory -Path $idea -Force | Out-Null
$vcsPath = Join-Path $idea 'vcs.xml'
$vcs = if (Test-Path -LiteralPath $vcsPath) { [xml](Get-Content -Raw -LiteralPath $vcsPath) } else { [xml]'<project version="4"><component name="VcsDirectoryMappings" /></project>' }
$manager = $vcs.SelectSingleNode('/project/component[@name="VcsDirectoryMappings"]')
if (!$manager) { $manager=$vcs.CreateElement('component'); $manager.SetAttribute('name','VcsDirectoryMappings'); $null=$vcs.DocumentElement.AppendChild($manager) }
if (!@($manager.SelectNodes('mapping') | Where-Object { $_.directory -eq $hostRoot }).Count) {
    $mapping=$vcs.CreateElement('mapping'); $mapping.SetAttribute('directory',$hostRoot); $mapping.SetAttribute('vcs','Git'); $null=$manager.AppendChild($mapping)
}
$vcs.Save($vcsPath)
& (Join-Path $PSScriptRoot 'Start-Demo.ps1') -HostProject $project -EngineRoot $EngineRoot -RiderHome $RiderHome
Write-Output 'In the separate Rider profile, open Commit and select Map_Review or WBP_Review. Only disposable host files were committed.'
