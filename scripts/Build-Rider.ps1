[CmdletBinding()]
param(
    [string]$RiderHome = "$env:LOCALAPPDATA\Programs\Rider",
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pluginMetadata = [xml](Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'rider\src\main\resources\META-INF\plugin.xml'))
$version = [string]$pluginMetadata.'idea-plugin'.version
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Expected a numeric major.minor.patch plugin version.' }
$buildRoot = Join-Path $taskRoot 'build'
$classes = Join-Path $buildRoot 'classes'
$javac = Join-Path $RiderHome 'jbr\bin\javac.exe'
$java = Join-Path $RiderHome 'jbr\bin\java.exe'
if (!(Test-Path -LiteralPath $javac)) { throw "No javac in Rider runtime: $RiderHome" }
$product = Get-Content -Raw -LiteralPath (Join-Path $RiderHome 'product-info.json') | ConvertFrom-Json
if ($product.productCode -ne 'RD' -or !$product.buildNumber.StartsWith('253.')) {
    throw 'This prototype targets Rider 2025.3 (build 253). Validate compatibility before changing the target.'
}
New-Item -ItemType Directory -Path $classes -Force | Out-Null
# Build into a fresh, uniquely named directory so removed classes cannot survive a rebuild.
$classes = Join-Path $buildRoot ('classes-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $classes -Force | Out-Null
$sources = @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'rider\src\main\java') -Filter '*.java' -Recurse -File)
$classpath = (Get-ChildItem -LiteralPath (Join-Path $RiderHome 'lib') -Filter '*.jar' -File | ForEach-Object FullName) -join ';'
$argumentsFile = Join-Path $buildRoot 'javac.args'
$compileArguments = @('--release', '21', '-encoding', 'UTF-8', '-Xlint:deprecation', '-classpath', $classpath, '-d', $classes) + @($sources.FullName)
$argumentText = ($compileArguments | ForEach-Object { '"' + $_.Replace('\', '/').Replace('"', '\"') + '"' }) -join "`n"
[IO.File]::WriteAllText($argumentsFile, $argumentText, [Text.UTF8Encoding]::new($false))
& $javac "@$argumentsFile"
if ($LASTEXITCODE -ne 0) { throw 'Rider plugin compilation failed.' }
$resources = Join-Path $taskRoot 'rider\src\main\resources'
Copy-Item -Path (Join-Path $resources '*') -Destination $classes -Recurse -Force
$distribution = Join-Path $buildRoot ('package-' + [guid]::NewGuid().ToString('N'))
$pluginDirectory = Join-Path $distribution 'unreal-asset-preview'
$pluginLib = Join-Path $pluginDirectory 'lib'
New-Item -ItemType Directory -Path $pluginLib -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRoot 'LICENSE') -Destination $pluginDirectory
# Rider bundles javac but some JBR distributions omit the jar CLI; JAR uses ordinary ZIP framing.
[IO.Compression.ZipFile]::CreateFromDirectory($classes, (Join-Path $pluginLib 'unreal-asset-preview.jar'))
$dist = Join-Path $taskRoot 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$archive = Join-Path $dist "unreal-asset-preview-$version.zip"
Compress-Archive -LiteralPath $pluginDirectory -DestinationPath $archive -Force
Write-Output "Compiled against Rider $($product.version) ($($product.buildNumber))."
Write-Output "Plugin: $archive"
if ($Test) {
    $testClasses = Join-Path $buildRoot ('tests-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testClasses -Force | Out-Null
    $testSource = Join-Path $taskRoot 'rider\src\test\java\dev\kaan\uassetpreview\BridgeClientTest.java'
    & $javac '--release' '21' '-encoding' 'UTF-8' '-classpath' "$classes;$classpath" '-d' $testClasses $testSource
    if ($LASTEXITCODE -ne 0) { throw 'Transport test compilation failed.' }
    & $java '-ea' '-Djava.awt.headless=true' '-classpath' "$testClasses;$classes;$classpath" 'dev.kaan.uassetpreview.BridgeClientTest'
    if ($LASTEXITCODE -ne 0) { throw 'Transport tests failed.' }
}
