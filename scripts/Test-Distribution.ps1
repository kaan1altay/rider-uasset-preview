#requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$installer = Join-Path $taskRoot 'Install.ps1'
$releaseRoot = Join-Path $taskRoot 'dist'
$manifest = Get-Content -Raw -LiteralPath (Join-Path $releaseRoot 'release-manifest.json') | ConvertFrom-Json
$fixtureRoot = Join-Path $taskRoot ('build\distribution-tests-' + [guid]::NewGuid().ToString('N'))
$fakeRider = Join-Path $fixtureRoot 'Rider installation'
$fakeEngine = Join-Path $fixtureRoot 'Unreal installation'
$profile = Join-Path $fixtureRoot 'Rider profile\plugins'
$projectRoot = Join-Path $fixtureRoot 'Example project'
$project = Join-Path $projectRoot 'Example.uproject'
$riderTarget = Join-Path $profile 'unreal-asset-preview'
$unrealTarget = Join-Path $projectRoot 'Plugins\RiderAssetPreview'
$originalLocalAppData = $env:LOCALAPPDATA
$script:distributionChecks = 0
$global:rapDistributionFailDestination = ''
$global:rapDistributionReleaseRoot = $releaseRoot
$global:rapDistributionManifest = $manifest
$global:rapDistributionRequests = [Collections.Generic.List[string]]::new()
$global:rapDistributionEditorActive = $false
$global:rapDistributionRiderActive = $false
$global:rapDistributionFakeRider = $fakeRider

function Assert-Distribution([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
    $script:distributionChecks++
}
function Expect-DistributionFailure([scriptblock]$Action, [string]$MessagePattern) {
    $failure = $null
    try { & $Action | Out-Null } catch { $failure = $_ }
    Assert-Distribution ($null -ne $failure -and $failure.ToString() -match $MessagePattern) "Expected failure matching: $MessagePattern; received: $failure"
}
# Inject a copy failure only for our test target to exercise rollback after the
# Rider install has succeeded and the previous Unreal directory has been moved.
function Copy-Item {
    [CmdletBinding()]
    param([string]$LiteralPath, [string]$Destination, [switch]$Recurse, [switch]$Force)
    if ($global:rapDistributionFailDestination -and $Destination -eq $global:rapDistributionFailDestination) {
        $global:rapDistributionFailDestination = ''
        throw 'Injected distribution test copy failure.'
    }
    Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
}
# Process state is simulated only inside these disposable file-install fixtures.
# Production Install.ps1 still checks real applications when run normally.
function Get-Process {
    [CmdletBinding()]
    param([string[]]$Name)
    if($global:rapDistributionEditorActive){[pscustomobject]@{ProcessName='UnrealEditor';Id=-1}}
}
function Get-CimInstance {
    [CmdletBinding()]
    param([string]$ClassName,[string]$Filter)
    if($global:rapDistributionRiderActive){[pscustomobject]@{ExecutablePath=(Join-Path $global:rapDistributionFakeRider 'bin\rider64.exe')}}
}
# Exercise the online code path without publishing a release or contacting GitHub.
# Only the exact API/asset URLs expected for this fixture are accepted.
function Invoke-RestMethod {
    param([string]$Uri, [hashtable]$Headers, [int]$TimeoutSec)
    if ($Uri -ne "https://api.github.com/repos/$($global:rapDistributionManifest.repository)/releases/latest") { throw 'Unexpected test API URL.' }
    $global:rapDistributionRequests.Add($Uri)
    return @{tag_name=('v' + $global:rapDistributionManifest.version)}
}
function Invoke-WebRequest {
    param([string]$Uri, [string]$OutFile, [int]$TimeoutSec)
    $expectedBase = "https://github.com/$($global:rapDistributionManifest.repository)/releases/download/v$($global:rapDistributionManifest.version)/"
    if (!$Uri.StartsWith($expectedBase) -or $Uri.Substring($expectedBase.Length) -match '[/\\]') { throw 'Unexpected test download URL.' }
    $global:rapDistributionRequests.Add($Uri)
    Copy-Item -LiteralPath (Join-Path $global:rapDistributionReleaseRoot $Uri.Substring($expectedBase.Length)) -Destination $OutFile
}
try {
    New-Item -ItemType Directory -Path $fakeRider,$projectRoot,(Join-Path $fakeEngine 'Engine\Build'),(Join-Path $fakeEngine 'Engine\Binaries\Win64') -Force | Out-Null
    $env:LOCALAPPDATA = Join-Path $fixtureRoot 'LocalAppData'
    @{productCode='RD'; buildNumber='253.31033.136'; dataDirectoryName='Rider2025.3'} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $fakeRider 'product-info.json')
    $engineParts = $manifest.unreal.Split('.')
    @{MajorVersion=[int]$engineParts[0]; MinorVersion=[int]$engineParts[1]; PatchVersion=[int]$engineParts[2]} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $fakeEngine 'Engine\Build\Build.version')
    @{BuildId=$manifest.unrealBuildId} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $fakeEngine 'Engine\Binaries\Win64\UnrealEditor.modules')
    @{FileVersion=3; EngineAssociation=($engineParts[0] + '.' + $engineParts[1]); Description='Keep this value'; Plugins=@(@{Name='OtherPlugin'; Enabled=$false})} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $project
    $installArguments = @{ReleaseDirectory=$releaseRoot; RiderHome=$fakeRider; RiderPluginsDirectory=$profile; EngineRoot=$fakeEngine; Project=$project}

    $repositoryXml = [xml](Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'updatePlugins.xml'))
    $releaseXml = [xml](Get-Content -Raw -LiteralPath (Join-Path $releaseRoot 'updatePlugins.xml'))
    Assert-Distribution ($repositoryXml.OuterXml -eq $releaseXml.OuterXml) 'Checked-in and packaged repository XML differ.'
    Assert-Distribution ($releaseXml.plugins.plugin.id -eq $manifest.pluginId) 'Repository plugin ID mismatch.'
    Assert-Distribution ($releaseXml.plugins.plugin.version -eq $manifest.version) 'Repository plugin version mismatch.'
    Assert-Distribution ($releaseXml.plugins.plugin.url -eq "https://github.com/$($manifest.repository)/releases/download/v$($manifest.version)/unreal-asset-preview-$($manifest.version).zip") 'Repository release URL mismatch.'
    Assert-Distribution ((Get-FileHash $installer).Hash -eq (Get-FileHash (Join-Path $releaseRoot 'Install.ps1')).Hash) 'Packaged installer is stale.'

    & $installer @installArguments -RiderOnly -WhatIf | Out-Null
    Assert-Distribution (!(Test-Path -LiteralPath $riderTarget)) 'WhatIf wrote a plugin directory.'
    & $installer @installArguments -RiderOnly | Out-Null
    Assert-Distribution (Test-Path -LiteralPath (Join-Path $riderTarget 'lib\unreal-asset-preview.jar')) 'Rider plugin was not installed.'
    Set-Content -LiteralPath (Join-Path $riderTarget 'preserve-marker.txt') -Value 'previous installation'
    & $installer @installArguments -RiderOnly | Out-Null
    $backups = @(Get-ChildItem -LiteralPath $env:LOCALAPPDATA -Filter 'preserve-marker.txt' -Recurse)
    Assert-Distribution ($backups.Count -eq 1) 'Update did not preserve the existing installation outside the plugin directory.'
    Assert-Distribution (!(Test-Path -LiteralPath (Join-Path $riderTarget 'preserve-marker.txt'))) 'Update retained obsolete plugin files.'
    $onlineArguments = $installArguments.Clone()
    $onlineArguments.Remove('ReleaseDirectory')
    & $installer @onlineArguments -RiderOnly | Out-Null
    Assert-Distribution ($global:rapDistributionRequests.Count -eq 3) 'Latest release did not resolve once and download manifest plus Rider archive from the fixed tag.'

    $badRelease = Join-Path $fixtureRoot 'bad release'
    New-Item -ItemType Directory -Path $badRelease | Out-Null
    $badManifest = Get-Content -Raw -LiteralPath (Join-Path $releaseRoot 'release-manifest.json') | ConvertFrom-Json
    $riderFile = @($badManifest.files | Where-Object kind -EQ 'rider')[0]
    Copy-Item -LiteralPath (Join-Path $releaseRoot $riderFile.name) -Destination (Join-Path $badRelease $riderFile.name)
    $riderFile.sha256 = '0' * 64
    $badManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $badRelease 'release-manifest.json')
    $badArguments = $installArguments.Clone()
    $badArguments.ReleaseDirectory = $badRelease
    $before = (Get-FileHash (Join-Path $riderTarget 'lib\unreal-asset-preview.jar')).Hash
    Expect-DistributionFailure { & $installer @badArguments -RiderOnly } 'integrity check failed'
    Assert-Distribution ((Get-FileHash (Join-Path $riderTarget 'lib\unreal-asset-preview.jar')).Hash -eq $before) 'Hash failure changed the installed plugin.'

    # An archive with a correct hash must still fail if its paths escape staging.
    $evilArchive = Join-Path $badRelease 'unsafe.zip'
    $evilZip = [IO.Compression.ZipFile]::Open($evilArchive, [IO.Compression.ZipArchiveMode]::Create)
    try { $null = $evilZip.CreateEntry('unreal-asset-preview/../escaped.txt') } finally { $evilZip.Dispose() }
    $riderFile.name = 'unsafe.zip'
    $riderFile.sha256 = (Get-FileHash -LiteralPath $evilArchive).Hash
    $riderFile.bytes = (Get-Item -LiteralPath $evilArchive).Length
    $badManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $badRelease 'release-manifest.json')
    Expect-DistributionFailure { & $installer @badArguments -RiderOnly } 'Unsafe or duplicate archive entry'
    Assert-Distribution (!(Test-Path -LiteralPath (Join-Path $fixtureRoot 'escaped.txt'))) 'Unsafe archive wrote outside staging.'
    @{productCode='RD'; buildNumber='252.1'; dataDirectoryName='Rider2025.2'} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $fakeRider 'product-info.json')
    Expect-DistributionFailure { & $installer @installArguments -RiderOnly } 'outside this release range'
    foreach($futureBuild in @('263.1','999.1')) {
        @{productCode='RD'; buildNumber=$futureBuild; dataDirectoryName='RiderFuture'} | ConvertTo-Json |
            Set-Content -LiteralPath (Join-Path $fakeRider 'product-info.json')
        Expect-DistributionFailure { & $installer @installArguments -RiderOnly } 'outside this release range'
    }
    foreach($supportedBuild in @('261.1','262.11879.119')) {
        @{productCode='RD'; buildNumber=$supportedBuild; dataDirectoryName='Rider2026.2'} | ConvertTo-Json |
            Set-Content -LiteralPath (Join-Path $fakeRider 'product-info.json')
        & $installer @installArguments -RiderOnly | Out-Null
        Assert-Distribution (Test-Path -LiteralPath (Join-Path $riderTarget 'lib\unreal-asset-preview.jar')) "Supported build $supportedBuild was rejected."
    }
    @{productCode='RD'; buildNumber='253.31033.136'; dataDirectoryName='Rider2025.3'} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $fakeRider 'product-info.json')

    $badManifest = Get-Content -Raw -LiteralPath (Join-Path $releaseRoot 'release-manifest.json') | ConvertFrom-Json
    $badManifest.riderSinceBuild = 'invalid'
    $badManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $badRelease 'release-manifest.json')
    Expect-DistributionFailure { & $installer @badArguments -RiderOnly } 'Invalid Rider compatibility range'
    $badManifest.riderSinceBuild = '252'
    $badManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $badRelease 'release-manifest.json')
    Expect-DistributionFailure { & $installer @badArguments -RiderOnly } 'archive compatibility range differs'

    $global:rapDistributionEditorActive=$true
    Expect-DistributionFailure { & $installer @installArguments -UnrealOnly } 'Close Unreal Editor'
    $global:rapDistributionEditorActive=$false
    Assert-Distribution (!(Test-Path -LiteralPath $unrealTarget)) 'Active Editor guard installed files.'
    $global:rapDistributionRiderActive=$true
    Expect-DistributionFailure { & $installer @installArguments -RiderOnly } 'Close Rider'
    $global:rapDistributionRiderActive=$false
    & $installer @installArguments | Out-Null
    Assert-Distribution (Test-Path -LiteralPath (Join-Path $unrealTarget 'Binaries\Win64\UnrealEditor-RiderAssetPreview.dll')) 'Unreal companion was not installed.'
    $updatedProject = Get-Content -Raw -LiteralPath $project | ConvertFrom-Json
    Assert-Distribution ($updatedProject.Description -eq 'Keep this value') 'Installer changed unrelated project metadata.'
    Assert-Distribution (@($updatedProject.Plugins | Where-Object { $_.Name -eq 'OtherPlugin' -and !$_.Enabled }).Count -eq 1) 'Installer changed another plugin.'
    Assert-Distribution (@($updatedProject.Plugins | Where-Object { $_.Name -eq 'RiderAssetPreview' -and $_.Enabled }).Count -eq 1) 'Companion plugin was not enabled exactly once.'

    Set-Content -LiteralPath (Join-Path $riderTarget 'rollback-marker.txt') -Value 'rider original'
    Set-Content -LiteralPath (Join-Path $unrealTarget 'rollback-marker.txt') -Value 'unreal original'
    $originalProjectHash = (Get-FileHash -LiteralPath $project).Hash
    $global:rapDistributionFailDestination = $unrealTarget
    Expect-DistributionFailure { & $installer @installArguments } 'Injected distribution test copy failure'
    Assert-Distribution (Test-Path -LiteralPath (Join-Path $riderTarget 'rollback-marker.txt')) 'Rider rollback did not restore the original plugin.'
    Assert-Distribution (Test-Path -LiteralPath (Join-Path $unrealTarget 'rollback-marker.txt')) 'Unreal rollback did not restore the original plugin.'
    Assert-Distribution ((Get-FileHash -LiteralPath $project).Hash -eq $originalProjectHash) 'Rollback changed the original project file.'

    @{BuildId='different-build'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fakeEngine 'Engine\Binaries\Win64\UnrealEditor.modules')
    Expect-DistributionFailure { & $installer @installArguments -UnrealOnly } 'does not match this Engine version/build'
    Write-Output "PASS: $script:distributionChecks distribution checks; offline install, simulated GitHub downloads, WhatIf, backups, rollback, integrity, archive paths, compatibility and repository metadata."
    Write-Output "Fixtures: $fixtureRoot"
} finally {
    $env:LOCALAPPDATA = $originalLocalAppData
    Remove-Variable -Name rapDistributionFailDestination -Scope Global -ErrorAction SilentlyContinue
    Remove-Variable -Name rapDistributionReleaseRoot,rapDistributionManifest,rapDistributionRequests -Scope Global -ErrorAction SilentlyContinue
    Remove-Variable -Name rapDistributionEditorActive,rapDistributionRiderActive,rapDistributionFakeRider -Scope Global -ErrorAction SilentlyContinue
}
