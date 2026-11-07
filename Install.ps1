#requires -Version 7.0
<#
.SYNOPSIS
Installs a GitHub Release into a closed Rider profile and an Unreal project.
.EXAMPLE
./Install.ps1 -Project 'D:\Games\MyGame\MyGame.uproject'
.EXAMPLE
./Install.ps1 -ReleaseDirectory ./dist -Project ./MyGame/MyGame.uproject -WhatIf
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Project,
    [string]$RiderHome = "$env:LOCALAPPDATA\Programs\Rider",
    [string]$RiderPluginsDirectory,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [string]$Repository = 'kaan1altay/rider-uasset-preview',
    [string]$Version = 'latest',
    [string]$ReleaseDirectory,
    [switch]$RiderOnly,
    [switch]$UnrealOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$IsWindows -or ![Environment]::Is64BitOperatingSystem) { throw 'This installer supports Windows x64 only.' }
if ($RiderOnly -and $UnrealOnly) { throw 'Choose either -RiderOnly or -UnrealOnly, not both.' }
$installRider = !$UnrealOnly
$installUnreal = !$RiderOnly
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Expected GitHub owner/repository.' }
if ($Version -ne 'latest' -and $Version -notmatch '^v?\d+\.\d+\.\d+$') { throw 'Expected latest or a major.minor.patch release version.' }

function Assert-NoLinks([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Install paths may not contain junctions or symbolic links: $cursor"
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}

function Expand-CheckedZip([string]$Archive, [string]$Destination, [string]$RootName) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $expandedBytes = 0L
        if ($zip.Entries.Count -gt 5000) { throw 'Archive contains too many entries.' }
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\','/')
            if ($name -notmatch ('^' + [regex]::Escape($RootName) + '/') -or $name -match '(^|/)\.\.?(/|$)|:|[<>"|?*]' -or
                $name -match '(^|/)[^/]*[. ](/|$)' -or !$seen.Add($name)) { throw 'Unsafe or duplicate archive entry.' }
            if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Symbolic links are not allowed in install archives.' }
            $expandedBytes += $entry.Length
            if ($expandedBytes -gt 256MB) { throw 'Expanded archive exceeds the size limit.' }
            $resolved = [IO.Path]::GetFullPath((Join-Path $Destination $name))
            if (!$resolved.StartsWith([IO.Path]::GetFullPath($Destination) + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Archive path escapes staging.' }
        }
        [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Destination)
    } finally { $zip.Dispose() }
}

# Validate targets before downloading anything. Never close an existing application.
if ($installRider) {
    $RiderHome = [IO.Path]::GetFullPath($RiderHome)
    $product = Get-Content -Raw -LiteralPath (Join-Path $RiderHome 'product-info.json') | ConvertFrom-Json
    if ($product.productCode -ne 'RD') { throw '-RiderHome must point to a Rider installation.' }
    if (!$RiderPluginsDirectory) {
        if ([string]$product.dataDirectoryName -notmatch '^Rider[\d.]+$') { throw 'Cannot determine the Rider profile. Pass -RiderPluginsDirectory explicitly.' }
        $RiderPluginsDirectory = Join-Path $env:APPDATA ('JetBrains\' + $product.dataDirectoryName + '\plugins')
    }
    $riderTarget = Join-Path ([IO.Path]::GetFullPath($RiderPluginsDirectory)) 'unreal-asset-preview'
    Assert-NoLinks $riderTarget
    $activeRider = @(Get-CimInstance Win32_Process -Filter "Name='rider64.exe' OR Name='java.exe'" | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($RiderHome + '\', [StringComparison]::OrdinalIgnoreCase)
    })
    if ($activeRider.Count) { throw 'Close Rider before installing into this installation/profile. No files were installed.' }
}
if ($installUnreal) {
    if (!$Project -or [IO.Path]::GetExtension($Project) -ne '.uproject' -or !(Test-Path -LiteralPath $Project -PathType Leaf)) {
        throw 'Pass an existing .uproject file with -Project, or use -RiderOnly.'
    }
    if (@(Get-Process UnrealEditor,UnrealEditor-Cmd,LiveCodingConsole -ErrorAction SilentlyContinue).Count) {
        throw 'Close Unreal Editor and Live Coding before installing. No files were installed.'
    }
    $Project = [IO.Path]::GetFullPath($Project)
    Assert-NoLinks $Project
    $projectData = Get-Content -Raw -LiteralPath $Project | ConvertFrom-Json -AsHashtable
    $EngineRoot = [IO.Path]::GetFullPath($EngineRoot)
    $engineVersion = Get-Content -Raw -LiteralPath (Join-Path $EngineRoot 'Engine\Build\Build.version') | ConvertFrom-Json
    $engineModules = Get-Content -Raw -LiteralPath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.modules') | ConvertFrom-Json
    $association = [string]$projectData['EngineAssociation']
    if ($association -match '^\d+\.\d+$') {
        if ($association -ne "$($engineVersion.MajorVersion).$($engineVersion.MinorVersion)") { throw 'Project EngineAssociation does not match -EngineRoot.' }
    } elseif ($association) {
        $builds = Get-ItemProperty -LiteralPath 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
        $registered = if ($builds) { $builds.PSObject.Properties[$association] } else { $null }
        if (!$registered -or [IO.Path]::GetFullPath([string]$registered.Value).TrimEnd('\') -ne $EngineRoot.TrimEnd('\')) {
            throw 'Cannot resolve the project EngineAssociation to -EngineRoot. Fix the association or build the plugin from source.'
        }
    } else { throw 'The project has no EngineAssociation. Select its Unreal Engine version before installing.' }
    $unrealTarget = Join-Path ([IO.Path]::GetDirectoryName($Project)) 'Plugins\RiderAssetPreview'
    Assert-NoLinks $unrealTarget
}

$workRoot = Join-Path ([IO.Path]::GetTempPath()) ('RiderUAssetViewer-install-' + [guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($workRoot)
try {
    if ($ReleaseDirectory) {
        $assetsRoot = [IO.Path]::GetFullPath($ReleaseDirectory)
        $manifestPath = Join-Path $assetsRoot 'release-manifest.json'
    } else {
        $api = "https://api.github.com/repos/$Repository/releases/"
        $api += if ($Version -eq 'latest') { 'latest' } else { 'tags/v' + $Version.TrimStart('v') }
        try {
            $release = Invoke-RestMethod -Uri $api -Headers @{'User-Agent'='RiderUAssetViewer-Installer'} -TimeoutSec 30
        } catch { throw 'Release not found or unavailable. Publish a GitHub Release with all five distribution files first.' }
        if ([string]$release.tag_name -notmatch '^v\d+\.\d+\.\d+$') { throw 'Unexpected release tag.' }
        $releaseVersion = ([string]$release.tag_name).Substring(1)
        $downloadBase = "https://github.com/$Repository/releases/download/$($release.tag_name)/"
        $assetsRoot = $workRoot
        $manifestPath = Join-Path $workRoot 'release-manifest.json'
        Invoke-WebRequest -Uri ($downloadBase + 'release-manifest.json') -OutFile $manifestPath -TimeoutSec 60
    }
    if ((Get-Item -LiteralPath $manifestPath).Length -gt 64KB) { throw 'Release manifest is too large.' }
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.pluginId -ne 'dev.kaan.uassetpreview' -or $manifest.platform -ne 'Win64' -or
        $manifest.repository -ne $Repository -or $manifest.version -notmatch '^\d+\.\d+\.\d+$') { throw 'Unsupported release manifest.' }
    if (!$ReleaseDirectory -and $manifest.version -ne $releaseVersion) { throw 'Release tag and manifest versions differ.' }
    if ($Version -ne 'latest' -and $manifest.version -ne $Version.TrimStart('v')) { throw 'Requested version differs from manifest.' }
    if ($installRider -and ($manifest.riderSinceBuild -ne '253' -or $manifest.riderUntilBuild -ne '253.*' -or
        $product.buildNumber -notmatch '^253\.')) { throw 'This release requires Rider 2025.3 (build 253).' }
    if ($installUnreal) {
        $installedEngine = "$($engineVersion.MajorVersion).$($engineVersion.MinorVersion).$($engineVersion.PatchVersion)"
        if ($manifest.unreal -ne $installedEngine -or $manifest.unrealBuildId -ne $engineModules.BuildId) {
            throw 'The prebuilt Unreal plugin does not match this Engine version/build. Build from source instead.'
        }
    }
    $plans = [Collections.Generic.List[object]]::new()
    foreach ($kind in @('rider','unreal')) {
        if (($kind -eq 'rider' -and !$installRider) -or ($kind -eq 'unreal' -and !$installUnreal)) { continue }
        $files = @($manifest.files | Where-Object kind -EQ $kind)
        if ($files.Count -ne 1) { throw "Expected exactly one $kind archive." }
        $file = $files[0]
        if ($file.name -notmatch '^[A-Za-z0-9_.-]+\.zip$' -or $file.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
            $file.bytes -le 0 -or $file.bytes -gt 128MB) { throw 'Invalid archive metadata.' }
        $archive = Join-Path $assetsRoot $file.name
        if (!$ReleaseDirectory) { Invoke-WebRequest -Uri ($downloadBase + $file.name) -OutFile $archive -TimeoutSec 180 }
        if ((Get-Item -LiteralPath $archive).Length -ne $file.bytes -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $file.sha256) {
            throw "Archive integrity check failed: $($file.name). No files were installed."
        }
        $stage = Join-Path $workRoot $kind
        $rootName = if ($kind -eq 'rider') { 'unreal-asset-preview' } else { 'RiderAssetPreview' }
        Expand-CheckedZip $archive $stage $rootName
        $source = Join-Path $stage $rootName
        if ($kind -eq 'rider') {
            $jar = [IO.Compression.ZipFile]::OpenRead((Join-Path $source 'lib\unreal-asset-preview.jar'))
            try {
                $reader = [IO.StreamReader]::new($jar.GetEntry('META-INF/plugin.xml').Open())
                try { $plugin = [xml]$reader.ReadToEnd() } finally { $reader.Dispose() }
                if ($plugin.'idea-plugin'.id -ne $manifest.pluginId -or $plugin.'idea-plugin'.version -ne $manifest.version) { throw 'Rider archive identity/version mismatch.' }
            } finally { $jar.Dispose() }
            $target = $riderTarget
        } else {
            $plugin = Get-Content -Raw -LiteralPath (Join-Path $source 'RiderAssetPreview.uplugin') | ConvertFrom-Json
            $modules = Get-Content -Raw -LiteralPath (Join-Path $source 'Binaries\Win64\UnrealEditor.modules') | ConvertFrom-Json
            if ($plugin.VersionName -ne $manifest.version -or $modules.BuildId -ne $manifest.unrealBuildId -or
                !(Test-Path -LiteralPath (Join-Path $source 'Binaries\Win64\UnrealEditor-RiderAssetPreview.dll'))) { throw 'Unreal archive version/build mismatch.' }
            $target = $unrealTarget
        }
        $plans.Add(@{kind=$kind; source=$source; target=$target; moved=$false; copied=$false})
    }
    Write-Output "Verified release $($manifest.version)."
    if ($plans.Count -eq 2) {
        $first = [IO.Path]::GetFullPath($plans[0].target).TrimEnd('\') + '\'
        $second = [IO.Path]::GetFullPath($plans[1].target).TrimEnd('\') + '\'
        if ($first.StartsWith($second, [StringComparison]::OrdinalIgnoreCase) -or $second.StartsWith($first, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Rider and Unreal install targets must not overlap.'
        }
    }
    $action = if ($installUnreal) { 'Install plugins and enable the Unreal project plugin' } else { 'Install the Rider plugin' }
    if (!$PSCmdlet.ShouldProcess(($plans.target -join ', '), $action)) { return }
    $backupRoot = Join-Path $env:LOCALAPPDATA ('RiderUAssetViewer\Backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
    Assert-NoLinks $backupRoot
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    if ($installUnreal) { Copy-Item -LiteralPath $Project -Destination (Join-Path $backupRoot 'project.uproject') }
    try {
        foreach ($plan in $plans) {
            Assert-NoLinks $plan.target
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($plan.target)) -Force | Out-Null
            if (Test-Path -LiteralPath $plan.target) {
                Move-Item -LiteralPath $plan.target -Destination (Join-Path $backupRoot $plan.kind)
                $plan.moved = $true
            }
            $plan.copied = $true
            Copy-Item -LiteralPath $plan.source -Destination $plan.target -Recurse
        }
        if ($installUnreal) {
            $plugins = @($projectData['Plugins'] | Where-Object { $_ -and $_.Name -ne 'RiderAssetPreview' })
            $projectData.Plugins = $plugins + @(@{Name='RiderAssetPreview'; Enabled=$true})
            [IO.File]::WriteAllText($Project, ($projectData | ConvertTo-Json -Depth 100) + "`n", [Text.UTF8Encoding]::new($false))
        }
    } catch {
        # Preserve partial output for inspection, and put every original directory back.
        foreach ($plan in $plans) {
            Assert-NoLinks $plan.target
            if ($plan.copied -and (Test-Path -LiteralPath $plan.target)) {
                Move-Item -LiteralPath $plan.target -Destination (Join-Path $backupRoot ('failed-' + $plan.kind))
            }
            if ($plan.moved) { Move-Item -LiteralPath (Join-Path $backupRoot $plan.kind) -Destination $plan.target }
        }
        if ($installUnreal) { Copy-Item -LiteralPath (Join-Path $backupRoot 'project.uproject') -Destination $Project -Force }
        throw
    }
    foreach ($plan in $plans) { Write-Output "Installed $($plan.kind): $($plan.target)" }
    Write-Output "Backups: $backupRoot"
    Write-Output 'Start Rider and the Unreal project to preview assets. Add the repository URL in Rider for future Rider plugin updates:'
    Write-Output "https://github.com/$Repository/releases/latest/download/updatePlugins.xml"
    Write-Output 'Update the Unreal companion separately with this installer when updating to a matching release.'
} finally {
    # Remove only the unique installer staging directory created above.
    $resolvedWork = [IO.Path]::GetFullPath($workRoot)
    $tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($resolvedWork) -eq $tempParent -and [IO.Path]::GetFileName($resolvedWork) -match '^RiderUAssetViewer-install-[a-f0-9]{32}$') {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force -WhatIf:$false -Confirm:$false
    }
}
