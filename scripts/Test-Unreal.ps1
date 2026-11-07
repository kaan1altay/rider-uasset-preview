[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HostProject,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [switch]$KeepAlive,
    [switch]$AllowOtherProjects
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$hostFile = [IO.Path]::GetFullPath($HostProject)
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (!$hostFile.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $hostFile)) {
    throw 'Only an isolated host project created beneath this repository build directory may be tested.'
}
if (@(Get-Process UnrealEditor,UnrealEditor-Cmd,LiveCodingConsole -ErrorAction SilentlyContinue).Count -and !$AllowOtherProjects) {
    throw 'Close Unreal Editor/Live Coding first. This test will not touch an existing Editor session.'
}
if($AllowOtherProjects){& (Join-Path $PSScriptRoot 'Assert-HostIsolation.ps1') -HostProject $hostFile}
$output = Join-Path $taskRoot ('build\smoke-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$log = Join-Path $output 'Unreal.log'
$descriptor = Join-Path ([IO.Path]::GetDirectoryName($hostFile)) 'Saved\RiderAssetPreview\session.json'
# The previous unattended test may have been terminated without module shutdown.
# This belongs to our isolated build host; never reuse its stale authentication descriptor.
if (Test-Path -LiteralPath $descriptor) { Remove-Item -LiteralPath $descriptor }
$processInfo = [Diagnostics.ProcessStartInfo]::new()
$processInfo.FileName = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$processInfo.UseShellExecute = $false
$processInfo.CreateNoWindow = $true
$processInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$processInfo.RedirectStandardOutput = $true
$processInfo.RedirectStandardError = $true
foreach ($argument in @($hostFile, '-RenderOffscreen', '-unattended', '-nosplash', '-nosound', '-NoLiveCoding', '-RiderAssetPreviewPort=32189', "-abslog=$log")) {
    $processInfo.ArgumentList.Add($argument)
}
$process = [Diagnostics.Process]::Start($processInfo)
$process.BeginOutputReadLine(); $process.BeginErrorReadLine()
Write-Output "Started isolated offscreen Unreal test host (PID $($process.Id))."
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
    while (!(Test-Path -LiteralPath $descriptor)) {
        if ($process.HasExited) { throw "Test Editor exited with code $($process.ExitCode). Log: $log" }
        if ([DateTime]::UtcNow -gt $deadline) { throw "Timed out waiting for the Unreal bridge. Log: $log" }
        Start-Sleep -Seconds 1
    }
    $session = Get-Content -Raw -LiteralPath $descriptor | ConvertFrom-Json
    $headers = @{ Authorization = 'Bearer ' + $session.token }
    function Send-PreviewJson([string]$Route, [hashtable]$Body) {
        Invoke-RestMethod -Uri ($session.endpoint + '/rap/v1/' + $Route) -Method Post -Headers $headers -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 60
    }
    $report = [Collections.Generic.List[object]]::new()
    $fixtures = @(
        @{ Name = 'cube'; File = 'Engine\Content\BasicShapes\Cube.uasset'; Type = 'StaticMesh' },
        @{ Name = 'skeletal-cube'; File = 'Engine\Content\EngineMeshes\SkeletalCube.uasset'; Type = 'SkeletalMesh' },
        @{ Name = 'texture'; File = 'Engine\Content\EngineResources\DefaultTexture.uasset'; Type = 'Texture2D' },
        @{ Name = 'blueprint'; File = 'Engine\Content\EngineSky\BP_Sky_Sphere.uasset'; Type = 'Blueprint' }
    )
    foreach ($fixture in $fixtures) {
        $asset = Send-PreviewJson 'open' @{ filename = Join-Path $EngineRoot $fixture.File }
        if ($asset.type -ne $fixture.Type) { throw "Wrong asset type for $($fixture.Name): $($asset.type)" }
        try {
            $details = Send-PreviewJson 'details' @{ session = $asset.session; component = 'asset' }
            if ($details.source -ne 'UnrealDetails' -or !$details.properties.Count) { throw "Missing editor-visible details for $($fixture.Name)." }
            foreach ($component in $asset.components) {
                if ($component.parent -ne 'asset' -and $component.parent -notin @($asset.components.id)) {
                    throw "Unknown hierarchy parent for component $($component.name)."
                }
                $componentDetails = Send-PreviewJson 'details' @{ session = $asset.session; component = $component.id }
                if ($componentDetails.source -ne 'UnrealDetails' -or !$componentDetails.properties.Count) { throw "Missing editor-visible properties for component $($component.name)." }
            }
            $image = Join-Path $output ($fixture.Name + '.png')
            $camera = @{ session = $asset.session; yaw = 40; pitch = 20; zoom = 1; focus = 'asset' }
            Invoke-WebRequest -Uri ($session.endpoint + '/rap/v1/frame') -Method Post -Headers $headers -ContentType 'application/json' -Body ($camera | ConvertTo-Json -Compress) -OutFile $image -TimeoutSec 60
            $bytes = [IO.File]::ReadAllBytes($image)
            if ($bytes.Length -lt 100 -or [Convert]::ToHexString($bytes[0..7]) -ne '89504E470D0A1A0A') { throw "Invalid preview PNG: $image" }
            $colors = [Collections.Generic.HashSet[int]]::new()
            Add-Type -AssemblyName System.Drawing
            $bitmap = [Drawing.Bitmap]::new($image)
            try {
                for ($y = 0; $y -lt $bitmap.Height; $y += 8) {
                    for ($x = 0; $x -lt $bitmap.Width; $x += 8) { $null = $colors.Add($bitmap.GetPixel($x, $y).ToArgb()) }
                }
            } finally { $bitmap.Dispose() }
            if ($fixture.Name -in @('cube', 'skeletal-cube')) {
                if ($colors.Count -lt 3) { throw "The $($fixture.Name) viewport appears blank or uniform ($($colors.Count) sampled colors)." }
                $camera.yaw = 110
                $orbitImage = Join-Path $output ($fixture.Name + '-orbit.png')
                Invoke-WebRequest -Uri ($session.endpoint + '/rap/v1/frame') -Method Post -Headers $headers -ContentType 'application/json' -Body ($camera | ConvertTo-Json -Compress) -OutFile $orbitImage -TimeoutSec 60
                if ((Get-FileHash -LiteralPath $image).Hash -eq (Get-FileHash -LiteralPath $orbitImage).Hash) { throw 'Changing camera yaw did not change the rendered image.' }
            }
            $asset | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output ($fixture.Name + '-hierarchy.json')) -Encoding utf8
            $report.Add([pscustomobject]@{ asset = $fixture.Name; type = $asset.type; components = @($asset.components).Count; properties = @($details.properties).Count; pngBytes = $bytes.Length; sampledColors = $colors.Count; image = $image; notice = $asset.notice })
            Write-Output "PASS $($fixture.Name): $(@($asset.components).Count) components, $(@($details.properties).Count) properties, $($bytes.Length) PNG bytes."
        } finally { $null = Send-PreviewJson 'close' @{ session = $asset.session } }
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
    Write-Output "Real Unreal smoke test passed. Inspect preview PNGs: $output"
    if ($KeepAlive) {
        $stopFile = Join-Path $output 'stop-host'
        Write-Output "Keeping this isolated host available for Rider UI verification. Create $stopFile to stop (15 minute limit)."
        $stopDeadline = [DateTime]::UtcNow.AddMinutes(15)
        while (!$process.HasExited -and !(Test-Path -LiteralPath $stopFile) -and [DateTime]::UtcNow -lt $stopDeadline) { Start-Sleep -Seconds 1 }
    }
} catch {
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 35 }
    throw
} finally {
    # Stop only the unattended process created above for this disposable test host.
    if (!$process.HasExited) { $process.Kill(); $process.WaitForExit(10000) | Out-Null }
    $process.Dispose()
}
