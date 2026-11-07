# Development

## Repository layout

- `rider/src/main/`: Rider file editor, Swing UI and bridge client.
- `rider/src/test/`: HTTP transport contract checks.
- `unreal/RiderAssetPreview/`: companion Unreal editor plugin source.
- `scripts/`: Windows build, test, demo and packaging helpers.
- `Install.ps1`: standalone GitHub Release/offline installer for both plugins.
- `updatePlugins.xml`: Rider custom repository descriptor for the current version.
- `docs/screenshots/`: screenshots of the prototype running in a separate Rider profile.
- `build/` and `dist/`: generated local output, excluded from Git.

Run scripts in PowerShell 7 from the repository root. The defaults assume Rider under `%LOCALAPPDATA%/Programs/Rider` and Unreal under `C:/Program Files/Epic Games/UE_5.8`; pass `-RiderHome` or `-EngineRoot` to override them.

## Build and verify

```powershell
.\scripts\Build-Rider.ps1 -Test
# Close Unreal Editor and Live Coding first.
.\scripts\Build-Unreal.ps1
```

The Unreal build creates `build/unreal-host-<timestamp>-<id>/AssetPreviewHost.uproject` and a compiled plugin under `dist/`. You can reuse that host for a later build with `-HostProject`; it must remain inside this checkout's `build/` directory. The default build uses one compile action and no PCH to reduce memory pressure. `-MaxParallelActions` accepts values from 1 to 3.

Use the host path printed by the build for real Unreal smoke tests:

```powershell
$hostProject = (Get-ChildItem .\build\unreal-host-*\AssetPreviewHost.uproject |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
.\scripts\Test-Unreal.ps1 -HostProject $hostProject
```

This starts its own offscreen Editor, checks Static Mesh, Skeletal Mesh, Texture2D and Actor Blueprint fixtures, and stops its own process when finished. It validates the hierarchy, Details responses, PNG signatures, mesh image variation and camera changes. Reports and images are written to `build/smoke-<timestamp>/`. It refuses to run while an existing Unreal Editor or Live Coding session is active.

## Isolated interactive demo

After both plugins are built:

```powershell
.\scripts\Start-Demo.ps1 -HostProject $hostProject
```

This launches an offscreen Unreal host on port `32189` and a separate Rider profile under `build/rider-sandbox/`. It does not install into your normal Rider profile. Complete any first-run or project-trust prompts yourself. If Rider's first-run wizard resets the plugin directory, close only the test Rider window and rerun the demo script.

The demo opens a directory inside the test host. Use Rider's **File → Open** to open an Engine fixture such as `Engine/Content/BasicShapes/Cube.uasset` or `Engine/Content/EngineSky/BP_Sky_Sphere.uasset`, keeping it in the same project window so the bridge can be discovered.

To start only the Unreal host and check Blueprint Details:

```powershell
.\scripts\Start-Demo.ps1 -HostProject $hostProject -HostOnly
.\scripts\Test-BlueprintDetails.ps1 -HostProject $hostProject
```

The targeted Details test checks visible transform fields, material slots, readable property labels, hidden diagnostic rows and the Advanced preference. Evidence is written under `build/evidence/`.

The interactive demo remains running until you stop it:

```powershell
.\scripts\Stop-Demo.ps1
```

The stop script verifies the saved host's PID, process start time and project path before stopping it. Close the separate Rider test window normally. Neither script closes user-owned Editor or Rider sessions.

## Verification status

The prototype has passed 40 HTTP transport checks, a UE 5.8.4 Windows build, and real offscreen Unreal tests for all four supported asset types. Rider UI checks covered plugin loading, mesh and Blueprint tabs, hierarchy selection, Details refresh, orbit, zoom and framing. The Blueprint Details fixture produced 57 default rows and 180 rows with Advanced enabled.

These are results from the development environment, not a guarantee for other machines or all assets. Engine fixture data and row counts can vary by version. Full Blueprint Editor parity, complex custom components, other engine versions, and interactive Rider tests for skeletal meshes and textures remain unverified. Raw local logs, session files and engine assets are deliberately excluded from the repository.

## Release packaging

After building both plugins, pass the compiled Unreal package printed by `Build-Unreal.ps1`:

```powershell
.\scripts\Package-Release.ps1 -UnrealPackage '.\dist\RiderAssetPreview-UE5.8-<timestamp>'
```

This verifies that the compiled Unreal package contains the current plugin source, then produces:

- `dist/unreal-asset-preview-0.1.0.zip`
- `dist/RiderAssetPreview-UE5.8-0.1.0.zip`
- `dist/release-manifest.json` with SHA-256 hashes and sizes.
- `dist/updatePlugins.xml` for Rider's custom plugin repository.
- `dist/Install.ps1` for single-command installation.

Run `scripts/New-PluginRepository.ps1` when changing plugin versions, then `scripts/Test-Distribution.ps1` after packaging. See [RELEASING.md](RELEASING.md) for the complete release checklist. Release publication is a separate manual action.

To test the real packages without downloading a release, use a disposable project/profile:

```powershell
.\Install.ps1 -ReleaseDirectory .\dist -Project 'D:\Test\Test.uproject' -RiderPluginsDirectory 'D:\Test\RiderPlugins' -WhatIf
```

PDB files stay in the full local build output. Rider/Unreal binaries, generated hosts, test profiles and logs should not be committed. Use release attachments for installable ZIPs when publishing a release separately.

## Before publishing changes

1. Run the Rider build and transport checks. For Unreal source changes, also rebuild and run the relevant integration tests.
2. Inspect the GitHub Desktop change list. It should contain source, scripts, documentation and screenshots, without `build/`, `dist/`, `Saved/`, tokens or local IDE settings.
3. Keep compatibility claims and limitations in the README aligned with actual tests.
4. Review the diff, then commit and publish through your preferred Git client.
