#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HostProject,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [string]$RiderHome = "$env:LOCALAPPDATA\Programs\Rider"
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project=[IO.Path]::GetFullPath($HostProject)
if (!$project.StartsWith((Join-Path $taskRoot 'build')+'\',[StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($project) -ne 'AssetPreviewHost.uproject' -or !(Test-Path -LiteralPath $project)) { throw 'Expected an isolated AssetPreviewHost beneath this checkout build directory.' }
$hostRoot=[IO.Path]::GetDirectoryName($project)
$fixture=Get-Content -Raw -LiteralPath (Join-Path $hostRoot 'Saved\RiderAssetPreview\TestEvidence\fixture-paths.json') | ConvertFrom-Json
$original=[IO.Path]::GetFullPath([string]$fixture.original)
$before=[IO.Path]::GetFullPath([string]$fixture.before)
$after=[IO.Path]::GetFullPath([string]$fixture.after)
if (!$original.StartsWith((Join-Path $hostRoot 'Content')+'\',[StringComparison]::OrdinalIgnoreCase) -or
    !$before.StartsWith((Join-Path $hostRoot 'Saved\RiderAssetPreview\Diff')+'\',[StringComparison]::OrdinalIgnoreCase) -or
    !$after.StartsWith((Join-Path $hostRoot 'Saved\RiderAssetPreview\Diff')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture files must belong to the isolated host.' }
$null=Get-Command git -ErrorAction Stop
$marker=Join-Path $hostRoot '.git\rider-review-fixture'
if (Test-Path -LiteralPath (Join-Path $hostRoot '.git')) {
    if (!(Test-Path -LiteralPath $marker) -or (Get-Content -Raw -LiteralPath $marker).Trim() -ne $original) {
        throw 'The host already has a different Git repository/fixture. Use a fresh isolated build host; no Git files were changed.'
    }
} else {
    & git -C $hostRoot init --initial-branch=review
    if($LASTEXITCODE){throw 'Could not initialize the disposable fixture repository.'}
    $relative=[IO.Path]::GetRelativePath($hostRoot,$original).Replace('\','/')
    @('*','!.gitignore','!.gitattributes','!Content/','Content/*','!Content/RiderReviewFixtures/','Content/RiderReviewFixtures/*',('!'+$relative)) |
        Set-Content -LiteralPath (Join-Path $hostRoot '.gitignore') -Encoding utf8
    '*.uasset binary' | Set-Content -LiteralPath (Join-Path $hostRoot '.gitattributes') -Encoding utf8
    Copy-Item -LiteralPath $before -Destination $original -Force
    & git -C $hostRoot add -- .gitignore .gitattributes $relative
    if($LASTEXITCODE){throw 'Could not stage the disposable fixture.'}
    $emptyHooks=Join-Path $hostRoot 'Saved\ReviewEmptyHooks'
    New-Item -ItemType Directory -Path $emptyHooks -Force | Out-Null
    & git -C $hostRoot -c "core.hooksPath=$emptyHooks" -c user.name='Rider Asset Preview Tests' -c user.email='tests@example.invalid' commit -m 'Before: review fixture'
    if($LASTEXITCODE){throw 'Could not create the disposable fixture revision.'}
    $original | Set-Content -LiteralPath $marker -Encoding utf8
}
# A running preview may hold the current package open. Reopening the same demo
# must not rewrite an already correct working copy.
if (!(Test-Path -LiteralPath $original) -or
    (Get-FileHash -LiteralPath $after).Hash -ne (Get-FileHash -LiteralPath $original).Hash) {
    Copy-Item -LiteralPath $after -Destination $original -Force
}
$idea=Join-Path $hostRoot 'Saved\RiderUi\.idea'
New-Item -ItemType Directory -Path $idea -Force | Out-Null
$vcsPath=Join-Path $idea 'vcs.xml'
$vcs=if(Test-Path -LiteralPath $vcsPath){[xml](Get-Content -Raw -LiteralPath $vcsPath)}else{[xml]'<project version="4"><component name="VcsDirectoryMappings" /></project>'}
$manager=$vcs.SelectSingleNode('/project/component[@name="VcsDirectoryMappings"]')
foreach($legacy in @($vcs.SelectNodes('/project/component[@name="ProjectLevelVcsManager"]'))){$null=$vcs.DocumentElement.RemoveChild($legacy)}
if(!$manager){$manager=$vcs.CreateElement('component');$manager.SetAttribute('name','VcsDirectoryMappings');$null=$vcs.DocumentElement.AppendChild($manager)}
if(!@($manager.SelectNodes('mapping') | Where-Object { $_.directory -eq $hostRoot }).Count){
    $mapping=$vcs.CreateElement('mapping');$mapping.SetAttribute('directory',$hostRoot);$mapping.SetAttribute('vcs','Git');$null=$manager.AppendChild($mapping)
}
$vcs.Save($vcsPath)
& (Join-Path $PSScriptRoot 'Start-Demo.ps1') -HostProject $project -EngineRoot $EngineRoot -RiderHome $RiderHome
Write-Output 'In the separate Rider window, open Commit and double-click the modified BP_Review asset.'
Write-Output 'The real Git diff contains a scale change, an added component and a Blueprint input change. This repository exists only inside the disposable build host.'
