#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$HostProject)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project=[IO.Path]::GetFullPath($HostProject)
$root=[IO.Path]::GetDirectoryName($project)
if (!$project.StartsWith((Join-Path $taskRoot 'build')+'\',[StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($project) -ne 'AssetPreviewHost.uproject' -or !(Test-Path -LiteralPath $project -PathType Leaf)) {
    throw 'Concurrent work requires an existing isolated AssetPreviewHost under this checkout build directory.'
}
$data=Get-Content -Raw -LiteralPath $project | ConvertFrom-Json -AsHashtable
if (@($data['Modules']).Where({$_}).Count -gt 0 -or @($data['Plugins'] | Where-Object {$_.Enabled -and $_.Name -ne 'RiderAssetPreview'}).Count) {
    throw 'Concurrent work is restricted to a content-only host enabling only RiderAssetPreview.'
}
for($cursor=$root;$cursor;$cursor=[IO.Path]::GetDirectoryName($cursor)) {
    if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'The isolated host may not use junctions or symbolic links.'}
}
$editors=@(Get-Process -Name UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue)
$commands=@(Get-CimInstance Win32_Process -Filter "Name='UnrealEditor.exe' OR Name='UnrealEditor-Cmd.exe' OR Name='LiveCodingConsole.exe'")
foreach($process in $commands){
    if(!$process.CommandLine){
        # WMI may retain a just-exited unattended test for a moment. Only a
        # demonstrably exited process can be ignored; live/unknown ones fail closed.
        $running=Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
        if(!$running -or $running.HasExited){continue}
        throw 'Cannot verify an Unreal/Live Coding process command line. Close it before proceeding.'
    }
    if($process.CommandLine.Contains($project,[StringComparison]::OrdinalIgnoreCase) -or $process.CommandLine.Contains($root+'\',[StringComparison]::OrdinalIgnoreCase)){
        throw 'The isolated host is already running or has Live Coding active. Stop only that demo host before proceeding.'
    }
}
foreach($editor in $editors){
    # Access denial is an error, never evidence of isolation. Only read module paths.
    foreach($module in $editor.Modules){
        if(!$module.FileName){throw 'Cannot verify an Editor module path.'}
        if($module.FileName.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'An open Editor has a DLL loaded from this test host. No build/test was started.'}
    }
}
Write-Output "Verified host isolation against $($editors.Count) other Editor process(es). User projects remain untouched."
