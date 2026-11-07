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

For a previously built content-only host, `-AllowOtherProjects` on `Build-Unreal.ps1`, `Test-Unreal.ps1` or `Test-Revisions.ps1` verifies host isolation and permits another game project to remain open. It rejects a running target host, DLLs loaded from that host, unverifiable process state, extra project modules/plugins and junctions. Builds still prohibit engine changes and hot reload. Tests use separate loopback ports.

## Revision regression tests and Git demo

```powershell
.\scripts\Test-Revisions.ps1 -HostProject $hostProject -AllowOtherProjects
.\scripts\Start-ReviewDemo.ps1 -HostProject $hostProject
```

The native test creates and saves before/after Blueprint packages with a scale change, an added component and a changed Print String input. It checks isolated loads, settings, graph data, rendering and revision package GC cleanup. Results are written to `build/revisions-<timestamp>/`.

The review demo creates a local, no-remote Git repository only inside the test host. HEAD contains the before fixture; the working tree contains the after fixture. In the separate Rider window, open **Commit** and double-click the modified `BP_Review` asset. Existing unrelated Git repositories are rejected. Rerunning the same fixture is supported; use a fresh host when switching to a different generated fixture.

Review data flow, identity rules and comparison boundaries are documented in [ASSET_REVIEW.md](ASSET_REVIEW.md).

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

Version 0.2.0 passes 55 HTTP transport checks, 42 comparison/LFS/image checks, ten checks using Rider's actual diff request types and packaged extension registration, and 27 distribution checks. Loading the real native before/after fixture into the Java comparison model adds four cross-layer assertions: one added component, the Body scale change, one changed logic node and no false component removal. The Unreal Windows build and revision automation fixture pass, including independent historical defaults, shared-camera PNGs and GC reclamation of a closed revision package. Real offscreen Unreal smoke tests also pass for Static Mesh, Skeletal Mesh, Texture2D and Actor Blueprint.

Earlier Rider UI checks covered plugin loading, mesh and Blueprint tabs, hierarchy selection, Details refresh, orbit, zoom and framing. The Blueprint Details fixture produced 57 default rows and 180 rows with Advanced enabled. The 0.2.0 real Git Commit demo verifies native viewer selection, before/after rendering, component navigation and the Body scale values, plus wipe-slider and pixel-difference modes. Current screenshots come from that isolated Rider profile. Interactive added/deleted, Git History and LFS UI checks remain separate from the adapter and resolver regression coverage.

These are results from the development environment, not a guarantee for other machines or all assets. Engine fixture data and row counts can vary by version. Full Blueprint Editor parity, complex custom components, other engine versions, and interactive Rider tests for skeletal meshes and textures remain unverified. Raw local logs, session files and engine assets are deliberately excluded from the repository.

## Release packaging

After building both plugins, pass the compiled Unreal package printed by `Build-Unreal.ps1`:

```powershell
.\scripts\Package-Release.ps1 -UnrealPackage '.\dist\RiderAssetPreview-UE5.8-<timestamp>'
```

This verifies that the compiled Unreal package contains the current plugin source, then produces:

- `dist/unreal-asset-preview-0.2.0.zip`
- `dist/RiderAssetPreview-UE5.8-0.2.0.zip`
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
