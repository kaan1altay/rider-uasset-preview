#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Repository = 'kaan1altay/rider-uasset-preview',
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Expected GitHub owner/repository.' }
if (!$OutputPath) { $OutputPath = Join-Path $taskRoot 'updatePlugins.xml' }
$metadata = [xml](Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'rider\src\main\resources\META-INF\plugin.xml'))
$plugin = $metadata.'idea-plugin'
$version = [string]$plugin.version
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Expected a numeric major.minor.patch plugin version.' }
$url = "https://github.com/$Repository/releases/download/v$version/unreal-asset-preview-$version.zip"
$settings = [Xml.XmlWriterSettings]::new()
$settings.Indent = $true
$settings.Encoding = [Text.UTF8Encoding]::new($false)
$settings.NewLineChars = "`n"
$writer = [Xml.XmlWriter]::Create([IO.Path]::GetFullPath($OutputPath), $settings)
try {
    $writer.WriteStartDocument()
    $writer.WriteStartElement('plugins')
    $writer.WriteStartElement('plugin')
    $writer.WriteAttributeString('id', [string]$plugin.id)
    $writer.WriteAttributeString('url', $url)
    $writer.WriteAttributeString('version', $version)
    $writer.WriteStartElement('idea-version')
    $writer.WriteAttributeString('since-build', [string]$plugin.'idea-version'.'since-build')
    $writer.WriteAttributeString('until-build', [string]$plugin.'idea-version'.'until-build')
    $writer.WriteEndElement()
    $writer.WriteElementString('name', [string]$plugin.name)
    $writer.WriteEndElement()
    $writer.WriteEndElement()
    $writer.WriteEndDocument()
} finally { $writer.Dispose() }
[IO.File]::AppendAllText([IO.Path]::GetFullPath($OutputPath), "`n", [Text.UTF8Encoding]::new($false))
Write-Output "Rider plugin repository: $OutputPath"
