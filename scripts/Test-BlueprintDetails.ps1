[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HostProject,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8'
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$hostFile = [IO.Path]::GetFullPath($HostProject)
$allowed = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (!$hostFile.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $hostFile -PathType Leaf)) { throw 'Expected an existing isolated demo host.' }
$descriptor = Join-Path ([IO.Path]::GetDirectoryName($hostFile)) 'Saved\RiderAssetPreview\session.json'
$connection = Get-Content -Raw -LiteralPath $descriptor | ConvertFrom-Json
$headers = @{Authorization='Bearer ' + $connection.token}
function Invoke-DetailsApi([string]$Route,[hashtable]$Body) {
    Invoke-RestMethod -Uri ($connection.endpoint + '/rap/v1/' + $Route) -Method Post -Headers $headers -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 60
}
$asset = Invoke-DetailsApi 'open' @{filename=(Join-Path $EngineRoot 'Engine\Content\EngineSky\BP_Sky_Sphere.uasset')}
try {
    $mesh = @($asset.components | Where-Object type -EQ 'StaticMeshComponent')[0]
    if (!$mesh) { throw 'The Blueprint mesh fixture is missing.' }
    $normal = Invoke-DetailsApi 'details' @{session=$asset.session;component=$mesh.id;advanced=$false}
    $advanced = Invoke-DetailsApi 'details' @{session=$asset.session;component=$mesh.id;advanced=$true}
    if ($normal.source -ne 'UnrealDetails') { throw 'Details did not come from the Unreal PropertyEditor model.' }
    if (@($normal.properties | Where-Object category -EQ 'Object').Count) { throw 'Reflection diagnostic rows are still exposed.' }
    if (@($normal.properties | Where-Object property -EQ 'WireframeColorOverride').Count) { throw 'Advanced wireframe override leaked into the default view.' }
    $staticMesh = @($normal.properties | Where-Object property -EQ 'StaticMesh')
    if (!$staticMesh.Count -or $staticMesh[0].name -eq 'StaticMesh') { throw 'Static Mesh is missing or still uses a raw C++ label.' }
    foreach ($propertyName in @('RelativeLocation','RelativeRotation','RelativeScale3D')) {
        if (!@($normal.properties | Where-Object property -EQ $propertyName).Count) { throw "Visible Blueprint transform is missing: $propertyName" }
    }
    if (!@($normal.properties | Where-Object category -EQ 'Materials').Count) { throw 'Visible Blueprint material slots are missing.' }
    if (@($advanced.properties).Count -le @($normal.properties).Count) { throw 'Advanced does not expose additional visible rows.' }
    $evidence = Join-Path $taskRoot 'build\evidence'
    New-Item -ItemType Directory -Path $evidence -Force | Out-Null
    $normal | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $evidence 'blueprint-details-default.json') -Encoding utf8
    $advanced | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $evidence 'blueprint-details-advanced.json') -Encoding utf8
    Write-Output "PASS Blueprint Details: $(@($normal.properties).Count) normal rows, $(@($advanced.properties).Count) advanced rows."
    Write-Output 'PASS visible transforms, materials, editor labels and hidden diagnostic/advanced fields.'
} finally { $null = Invoke-DetailsApi 'close' @{session=$asset.session} }

foreach ($fixture in @('BasicShapes\Cube.uasset','EngineMeshes\SkeletalCube.uasset','EngineResources\DefaultTexture.uasset')) {
    $other = Invoke-DetailsApi 'open' @{filename=(Join-Path $EngineRoot ('Engine\Content\' + $fixture))}
    try {
        $rows = Invoke-DetailsApi 'details' @{session=$other.session;component='asset';advanced=$false}
        if ($rows.source -ne 'UnrealDetails' -or !@($rows.properties).Count) { throw "No editor-visible details for $fixture" }
        if (@($rows.properties | Where-Object category -EQ 'Object').Count) { throw "Diagnostic metadata leaked for $fixture" }
        Write-Output "PASS $fixture : $(@($rows.properties).Count) editor-visible detail rows."
    } finally { $null = Invoke-DetailsApi 'close' @{session=$other.session} }
}
