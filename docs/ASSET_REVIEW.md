# Asset review

The asset viewer and the diff viewer are separate Rider integrations. `AssetEditorProvider` handles local `.uasset` and `.umap` editor tabs. `AssetDiffTool` is a native `FrameDiffTool` for two-way VCS and file comparisons, including non-local historical files and empty added/deleted sides. Text diffs and three-way merges remain with Rider's other viewers.

## Data flow and ownership

1. `AssetDiffTool` reads Rider's diff contents and original VCS `FilePath`. It does not run `git checkout` or read the working file as a substitute for a historical side.
2. `RevisionBytes` validates package bytes. Git LFS pointers can resolve through the repository's local SHA-256-verified cache, including standard Git worktrees. Missing objects produce a recovery message; no fetch/network operation is initiated automatically.
3. Each side is staged in a unique directory below the open project's `Saved/RiderAssetPreview/Diff/`. A client-generated request ID allows cleanup even if opening a revision times out.
4. The Unreal companion uses `DiffUtils::LoadPackageForDiff`, with the original package path and an isolated `/Temp` package, to avoid replacing the live asset or generated class. Separate preview scenes render the two revisions.
5. `Inspection` parses immutable snapshots and compares stable component/property/node keys. It has no dependency on Swing or Rider services. `AssetKnowledge` supplies deterministic explanations and falls back to Unreal's own property tooltips.
6. `AssetDiffViewer` owns UI state on the EDT and serializes bridge/session work on one background executor. It coalesces camera requests and shares bounds between the two views. `RevisionImages` computes pixel differences off the UI thread. `ReviewTableModel` emits one update per filter change.
7. Closing a viewer releases its server sessions before removing acknowledged staging data. Revision packages release loaders and standalone retention after their preview references are gone; normal Unreal GC reclaims them. The cleanup path does not use `PackageTools::UnloadPackages`, which can reset editor selection/undo. If closure cannot be acknowledged, staging bytes are retained rather than deleted while Unreal may still be loading them.

## What is compared

- **Component structure:** additions/removals, names, types, attachment parents and sockets. SCS GUIDs identify Blueprint components across renames. Legacy missing GUIDs use a name fallback, so a legacy rename may appear as removal/addition. Synthetic mesh components have a fixed identity.
- **Editor-visible settings:** Unreal PropertyEditor visibility rules and stable property paths, including Advanced values in reviews. Display labels stay human-readable. Normalized full-value fingerprints distinguish precise or long values even when a preview is shortened.
- **Blueprint logic:** persistent graph/node identities, serialized node settings, pin types, defaults and links. Connection display uses readable node/pin names; the fingerprint uses stable identities. Layout coordinates, comments and compiler diagnostics are omitted. This is a read-only list of logic changes, not a Blueprint graph editor.
- **Derived facts:** source texture dimensions, mesh detail/geometry counts, skeletal bone counts and Blueprint parent class. These review facts are separate from the ordinary editable Details view.
- **Visual changes:** rendered side by side, wipe, single-side views and a pixel difference mask. Shared camera bounds preserve changes in scale/position. Different image dimensions are compared at a shared preview resolution; source dimensions remain available as a setting difference.

Search includes the owner component, category, setting, values and explanation. The all-changes table and copied report identify the owner so equally named settings on different components remain distinguishable. Missing/truncated exports are not treated as deleted properties or nodes.

For modified Blueprint nodes, changed pin/default lines appear first in the comparison; full node descriptions remain in the explanation. This keeps a changed input visible among otherwise unchanged node details.

## Boundaries

Map previews load a separate saved package even for ordinary editor tabs. Actor GUIDs and actor-relative component names supply hierarchy identities. Only the persistent level is inspected: World Partition external actors and streamed levels are excluded and marked partial. Rendering copies static/skeletal mesh geometry with Unreal-evaluated socket and absolute transforms into a neutral preview scene. It does not switch the active editor world, run gameplay, reconstruct lighting, or render landscape/BSP. Limits are 128 actors, 512 hierarchy nodes and 10,000 instances per mesh component. Omitted nodes in a partial hierarchy cannot establish additions/removals.

Widget Blueprint previews also stage saved bytes and instantiate a transient designer widget. Widget GUIDs identify controls; synthetic Layout children expose panel-slot settings. The 1280 × 720 canvas has no orbit/zoom. PreConstruct, Construct and OnInitialized are skipped, but paint/binding callbacks and native designer code may run. Nested widget templates are counted recursively before instantiation (512 controls, depth 16, no cycles); native custom widget code can still allocate resources. Nested widgets render but their internals are not expanded in the tree. Layout based on runtime initialization can differ from the game.

Dependencies, materials, parent Blueprints and native classes use their current versions in the open project. Both sides can therefore change appearance after a dependency change even if the inspected asset did not change. Loading a revision does not reconstruct an entire historical project or its code.

Construction Script, BeginPlay, child actor spawning and physics simulation are not run by the preview. Native constructors, package loading, component registration and rendering callbacks can still run; this is not a sandbox for untrusted project code or assets.

This release does not provide a complete binary, mesh topology, animation, texture-source or arbitrary DataTable-content diff. Equal inspected settings/counts do not prove equal assets. A review with no detected setting changes explicitly explains this scope instead of claiming the binary revisions are identical.

Per-side package size is limited to 128 MB. Inspection currently bounds component property export to 128 components and 3,000 setting rows, with up to 500 rows per object and 32 visible collection children. Full collection fingerprints can still detect changes outside the displayed children. Blueprint logic export includes up to 512 nodes. Display text, canonical previews and response size are bounded; the viewer reports incomplete coverage. Eight active preview sessions are shared between ordinary tabs and reviews, and idle sessions expire after 30 minutes.

Asset-authored strings are treated as plain text, including table cells, tree labels and tooltips. They cannot invoke Swing HTML image loading. The plugin's bridge traffic is authenticated loopback HTTP; asset data is not sent to a remote analysis service.

## Regression fixtures

`scripts/Test-Spatial.ps1` creates saved before/after map and Widget Blueprint packages in the disposable host. It checks actor addition/movement, absolute child transforms, real PNG rendering, isolated ordinary previews, active-world preservation, skipped Blueprint PreConstruct and closed-package GC. Pass its evidence directory to `Build-Rider.ps1 -Test -SpatialEvidence ...` to check the actual Unreal exports through the Java comparison model.

`scripts/Test-Revisions.ps1` invokes the native `RiderAssetPreview.Revisions.IsolatedBlueprint` automation test. Fixture generation is hard-restricted to `AssetPreviewHost.uproject`; it never writes into a user's game project. The fixture saves two actual Blueprint package versions with a scale change, a new component and a changed Print String input. It checks independent historical defaults, logic export, shared-camera images, the original current template, snapshot path rejection and GC cleanup.

After native tests pass, `scripts/Start-ReviewDemo.ps1` prepares a local Git repository inside that disposable host: HEAD contains the before fixture and the working tree contains the after fixture. It opens the existing separate Rider profile for a real Commit-view diff. This fixture repository has no remote and does not alter the plugin repository's Git index or history.

The Java tests cover comparison rules, partial coverage, ownership labels, fingerprints, LFS cache integrity, pixel comparison and Rider's actual diff request types. The bridge tests exercise authenticated HTTP requests, revision request IDs, shared camera bounds and failure handling. These tests complement, rather than replace, real Rider UI and Unreal checks.
