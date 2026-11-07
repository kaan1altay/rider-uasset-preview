#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$RiderHome,
    [Parameter(Mandatory)][string]$VerifierJar,
    [string]$PluginArchive
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$metadata = [xml](Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'rider\src\main\resources\META-INF\plugin.xml'))
if (!$PluginArchive) { $PluginArchive = Join-Path $taskRoot ('dist\unreal-asset-preview-' + $metadata.'idea-plugin'.version + '.zip') }
$PluginArchive = (Resolve-Path -LiteralPath $PluginArchive).Path
$VerifierJar = (Resolve-Path -LiteralPath $VerifierJar).Path
$reportRoot = Join-Path $taskRoot ('build\rider-compatibility-' + [guid]::NewGuid().ToString('N'))
foreach ($installation in $RiderHome) {
    $installation = (Resolve-Path -LiteralPath $installation).Path
    $productPath = Join-Path $installation 'product-info.json'
    $productHash = (Get-FileHash -LiteralPath $productPath).Hash
    $product = Get-Content -Raw -LiteralPath $productPath | ConvertFrom-Json
    if ($product.productCode -ne 'RD') { throw 'Expected a JetBrains Rider installation.' }
    $java = Join-Path $installation 'jbr\bin\java.exe'
    $reports = Join-Path $reportRoot $product.buildNumber
    New-Item -ItemType Directory -Path $reports -Force | Out-Null
    # Check the exact distributed binary. No IDE is launched, and no dependency is
    # downloaded implicitly. A verifier process exit code alone is not a verdict.
    & $java '-Xmx768m' '-jar' $VerifierJar 'check-plugin' $PluginArchive $installation '-offline' '-verification-reports-dir' $reports *> (Join-Path $reports 'verifier.log')
    if ($LASTEXITCODE -ne 0) { throw "Plugin Verifier failed. See $reports\verifier.log" }
    if ((Get-FileHash -LiteralPath $productPath).Hash -ne $productHash) { throw 'Rider changed during verification. Run against a stable, separate installation.' }
    $verdicts = @(Get-ChildItem -LiteralPath $reports -Recurse -File -Filter 'verification-verdict.txt')
    if ($verdicts.Count -ne 1) { throw "Expected exactly one compatibility verdict. See $reports" }
    foreach ($problem in @(Get-ChildItem -LiteralPath $reports -Recurse -File | Where-Object { $_.Name -in @('compatibility-problems.txt','invalid-plugin.txt','non-extendable-api-usages.txt','override-only-usages.txt') })) {
        if ($problem.Length -gt 0) { throw "Rider $($product.buildNumber) compatibility failure: $($problem.FullName)" }
    }
    $verdict = (Get-Content -Raw -LiteralPath $verdicts[0].FullName).Trim()
    if ($verdict -ne 'Compatible') { throw "Rider $($product.buildNumber) was not verified compatible: $verdict. See $reports" }
    Write-Output "Rider $($product.version) ($($product.buildNumber)): $verdict"
    Write-Output "Reports: $reports"
}
