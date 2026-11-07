[CmdletBinding()]
param(
    [string]$HostProject,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [string]$RiderHome = "$env:LOCALAPPDATA\Programs\Rider",
    [switch]$HostOnly
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pluginMetadata = [xml](Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'rider\src\main\resources\META-INF\plugin.xml'))
$riderArchive = Join-Path $taskRoot ('dist\unreal-asset-preview-' + $pluginMetadata.'idea-plugin'.version + '.zip')
$sandbox = Join-Path $taskRoot 'build\rider-sandbox'
$stateFile = Join-Path $taskRoot 'build\demo-state.json'
if (!$HostProject) {
    if (Test-Path -LiteralPath $stateFile) {
        $HostProject = [string](Get-Content -Raw -LiteralPath $stateFile | ConvertFrom-Json).project
    } else {
        $latestHost = Get-ChildItem -Path (Join-Path $taskRoot 'build\unreal-host-*\AssetPreviewHost.uproject') -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($latestHost) { $HostProject = $latestHost.FullName }
    }
}
if (!$HostProject) { throw 'No demo host found. Run Build-Unreal.ps1 first, then pass its host project with -HostProject.' }
$project = [IO.Path]::GetFullPath($HostProject)
$allowed = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (!$project.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $project -PathType Leaf)) {
    throw 'The demo requires an existing host project beneath this repository build directory.'
}
$hostRoot = [IO.Path]::GetDirectoryName($project)
if (!(Test-Path -LiteralPath (Join-Path $hostRoot 'Plugins\RiderAssetPreview\Binaries\Win64\UnrealEditor-RiderAssetPreview.dll'))) {
    throw 'The demo host plugin is not built. Run Build-Unreal.ps1 for this host first.'
}
if (!(Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'))) {
    throw 'UnrealEditor-Cmd.exe is missing. Check -EngineRoot.'
}
if (!$HostOnly) {
    if (!(Test-Path -LiteralPath (Join-Path $RiderHome 'bin\rider.bat')) -or
        !(Test-Path -LiteralPath $riderArchive)) {
        throw 'Build the Rider plugin first and check -RiderHome.'
    }
    New-Item -ItemType Directory -Path $sandbox -Force | Out-Null
    $profileProperties = @()
    foreach ($entry in @('config','system','plugins','log')) {
        $profileProperties += 'idea.' + $entry + '.path=' + (Join-Path $sandbox $entry).Replace('\','/')
    }
    $profileProperties += 'idea.initially.ask.config=false'
    # Generate checkout-relative profile paths rather than publishing machine-specific settings.
    if (!(Test-Path -LiteralPath (Join-Path $sandbox 'sandbox.properties'))) {
        $profileProperties | Set-Content -LiteralPath (Join-Path $sandbox 'sandbox.properties') -Encoding utf8
    }
    if (!(Test-Path -LiteralPath (Join-Path $sandbox 'sandbox.vmoptions'))) {
        $defaults = Get-Content -LiteralPath (Join-Path $RiderHome 'bin\rider64.exe.vmoptions')
        @($defaults) + @($profileProperties | ForEach-Object { '-D' + $_ }) |
            Set-Content -LiteralPath (Join-Path $sandbox 'sandbox.vmoptions') -Encoding utf8
    }
    New-Item -ItemType Directory -Path (Join-Path $hostRoot 'Saved\RiderUi') -Force | Out-Null
}
$matchingHosts = @(Get-CimInstance Win32_Process -Filter "Name='UnrealEditor-Cmd.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($project) })
if ($matchingHosts.Count -gt 1) { throw 'Multiple demo hosts are running; stop the unused demo hosts first.' }
$createdHost = $false
$hostProcess = $null
try {
    if ($matchingHosts.Count -eq 1) {
        $hostProcess = Get-Process -Id $matchingHosts[0].ProcessId
        Write-Output "Reusing isolated Unreal demo host PID $($hostProcess.Id)."
    } else {
        $descriptor = Join-Path $hostRoot 'Saved\RiderAssetPreview\session.json'
        if (Test-Path -LiteralPath $descriptor) { Remove-Item -LiteralPath $descriptor }
        $log = Join-Path $hostRoot 'Saved\Logs\PreviewDemo.log'
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($log)) -Force | Out-Null
        $arguments = @('"' + $project + '"', '-RenderOffscreen', '-unattended', '-nosplash', '-nosound', '-NoLiveCoding', '-RiderAssetPreviewPort=32189', '"-abslog=' + $log + '"')
        $hostProcess = Start-Process -FilePath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
        $createdHost = $true
        Write-Output "Started isolated Unreal demo host PID $($hostProcess.Id)."
    }
    $state = [ordered]@{
        hostPid = $hostProcess.Id
        hostStarted = $hostProcess.StartTime.ToUniversalTime().ToString('O')
        project = $project
        log = (Join-Path $hostRoot 'Saved\Logs\PreviewDemo.log')
    }
    $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding utf8
    $descriptor = Join-Path $hostRoot 'Saved\RiderAssetPreview\session.json'
    $deadline = [DateTime]::UtcNow.AddMinutes(3)
    while (!(Test-Path -LiteralPath $descriptor)) {
        $hostProcess.Refresh()
        if ($hostProcess.HasExited) { throw "Demo Unreal exited with code $($hostProcess.ExitCode). Inspect PreviewDemo.log." }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Timed out waiting for the Unreal demo bridge.' }
        Start-Sleep -Seconds 1
    }
    # Wait for the first authenticated request to be handled, without logging session credentials.
    $connection = Get-Content -Raw -LiteralPath $descriptor | ConvertFrom-Json
    $headers = @{ Authorization = 'Bearer ' + $connection.token }
    $request = @{filename=(Join-Path $EngineRoot 'Engine\Content\BasicShapes\Cube.uasset')} | ConvertTo-Json -Compress
    $preview = Invoke-RestMethod -Uri ($connection.endpoint + '/rap/v1/open') -Method Post -Headers $headers -ContentType 'application/json' -Body $request -TimeoutSec 60
    $null = Invoke-RestMethod -Uri ($connection.endpoint + '/rap/v1/close') -Method Post -Headers $headers -ContentType 'application/json' -Body (@{session=$preview.session} | ConvertTo-Json -Compress) -TimeoutSec 30
    Write-Output 'Unreal demo bridge is ready; verified Cube asset loading.'
    if ($HostOnly) {
        Write-Output 'The isolated demo host is ready. Start-Demo.ps1 without -HostOnly also opens Rider.'
        return
    }

    $runningSandbox = @(Get-CimInstance Win32_Process -Filter "Name='java.exe'" | Where-Object {
        $_.CommandLine -and ($_.CommandLine.Contains($sandbox) -or $_.CommandLine.Contains($sandbox.Replace('\','/')))
    })
    if ($runningSandbox.Count -eq 0) {
        $plugins = Join-Path $sandbox 'plugins'
        New-Item -ItemType Directory -Path $plugins -Force | Out-Null
        Expand-Archive -LiteralPath $riderArchive -DestinationPath $plugins -Force
        $ui = Join-Path $hostRoot 'Saved\RiderUi'
        $bat = Join-Path $RiderHome 'bin\rider.bat'
        $arguments = '/d /c ""' + $bat + '" "' + $ui + '""'
        $previousProperties = $env:RIDER_PROPERTIES
        $previousVmOptions = $env:RIDER_VM_OPTIONS
        try {
            $env:RIDER_PROPERTIES = Join-Path $sandbox 'sandbox.properties'
            $env:RIDER_VM_OPTIONS = Join-Path $sandbox 'sandbox.vmoptions'
            $launcher = Start-Process -FilePath $env:ComSpec -ArgumentList $arguments -WindowStyle Hidden -RedirectStandardOutput (Join-Path $sandbox 'demo-launch-out.log') -RedirectStandardError (Join-Path $sandbox 'demo-launch-error.log') -PassThru
        } finally {
            $env:RIDER_PROPERTIES = $previousProperties
            $env:RIDER_VM_OPTIONS = $previousVmOptions
        }
        $state.riderLauncherPid = $launcher.Id
        $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding utf8
        Write-Output "Started the separate Rider test profile (launcher PID $($launcher.Id))."
    } else { Write-Output 'The separate Rider test profile is already running.' }
    Write-Output 'The demo remains open until you close it. Stop the background host with scripts\Stop-Demo.ps1.'
} catch {
    if ($createdHost -and $hostProcess -and !$hostProcess.HasExited) { Stop-Process -Id $hostProcess.Id -Force }
    throw
}
