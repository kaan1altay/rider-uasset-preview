[CmdletBinding()]
param(
    [string]$RiderHome = "$env:LOCALAPPDATA\Programs\Rider",
    [string]$ReviewEvidence,
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
$minimumBuild = [string]$pluginMetadata.'idea-plugin'.'idea-version'.'since-build'
$maximumBuild = [string]$pluginMetadata.'idea-plugin'.'idea-version'.'until-build'
if ($minimumBuild -notmatch '^\d{3}$' -or $maximumBuild -notmatch '^\d{3}\.\*$') {
    throw 'Expected branch-based Rider build bounds in plugin.xml.'
}
$branch = ([string]$product.buildNumber).Split('.')[0]
if ($product.productCode -ne 'RD' -or $branch -notmatch '^\d{3}$' -or
    [int]$branch -lt [int]$minimumBuild -or [int]$branch -gt [int]$maximumBuild.Split('.')[0]) {
    throw "Rider build $($product.buildNumber) is outside the plugin range $minimumBuild to $maximumBuild."
}
New-Item -ItemType Directory -Path $classes -Force | Out-Null
# Build into a fresh, uniquely named directory so removed classes cannot survive a rebuild.
$classes = Join-Path $buildRoot ('classes-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $classes -Force | Out-Null
$sources = @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'rider\src\main\java') -Filter '*.java' -Recurse -File)
$classpath = (Get-ChildItem -LiteralPath (Join-Path $RiderHome 'lib') -Filter '*.jar' -File | ForEach-Object FullName) -join ';'
$vcsApi = Join-Path $RiderHome 'lib\modules\intellij.platform.vcs.impl.jar'
if (!(Test-Path -LiteralPath $vcsApi)) {
    # Rider 2026.2 moved this module into the top-level library directory.
    $vcsApi = Join-Path $RiderHome 'lib\intellij.platform.vcs.impl.jar'
}
if (!(Test-Path -LiteralPath $vcsApi)) { throw 'The Rider VCS diff API is missing from this installation.' }
$classpath += ';' + $vcsApi
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
    $testSources = @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'rider\src\test\java') -Filter '*.java' -Recurse -File | ForEach-Object FullName)
    $testArgumentsFile = Join-Path $buildRoot 'javac-tests.args'
    $testArguments = @('--release', '21', '-encoding', 'UTF-8', '-classpath', "$classes;$classpath", '-d', $testClasses) + $testSources
    $testArgumentText = ($testArguments | ForEach-Object { '"' + $_.Replace('\', '/').Replace('"', '\"') + '"' }) -join "`n"
    [IO.File]::WriteAllText($testArgumentsFile, $testArgumentText, [Text.UTF8Encoding]::new($false))
    & $javac "@$testArgumentsFile"
    if ($LASTEXITCODE -ne 0) { throw 'Transport test compilation failed.' }
    function Invoke-PluginJavaTest([string]$MainClass, [string[]]$ExtraArguments = @()) {
        $runFile = Join-Path $buildRoot 'java-test.args'
        $runArguments = @('-ea', '-Djava.awt.headless=true', '-classpath', "$testClasses;$classes;$classpath", $MainClass) + $ExtraArguments
        $runText = ($runArguments | ForEach-Object { '"' + $_.Replace('\', '/').Replace('"', '\"') + '"' }) -join "`n"
        [IO.File]::WriteAllText($runFile, $runText, [Text.UTF8Encoding]::new($false))
        & $java "@$runFile"
        if ($LASTEXITCODE -ne 0) { throw "Java regression test failed: $MainClass" }
    }
    Invoke-PluginJavaTest 'dev.kaan.uassetpreview.BridgeClientTest'
    Invoke-PluginJavaTest 'dev.kaan.uassetpreview.InspectionTest'
    if($ReviewEvidence){
        Invoke-PluginJavaTest 'dev.kaan.uassetpreview.InspectionTest' @((Join-Path $ReviewEvidence 'before.json'), (Join-Path $ReviewEvidence 'after.json'))
    }
    Invoke-PluginJavaTest 'dev.kaan.uassetpreview.AssetDiffToolTest'
}
