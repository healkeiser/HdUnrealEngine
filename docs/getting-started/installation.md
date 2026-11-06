# Installation

hdUE ships as one folder plus a Houdini package file, the way HtoA or Redshift install. You build the folder once on a machine with the Unreal Engine source, then copy it to each workstation or render node.

!!! warning "Unreal Engine licensees only"
    The package contains Unreal Engine Developer modules, which the Unreal Engine EULA calls Engine Tools. Section 6(d) allows sharing such a package only through Fab or a fork of Epic's UnrealEngine GitHub network: inside your studio, or with other Unreal Engine licensees. Never publish it as a public download. There is no prebuilt download for this reason: you build your own package.

## Requirements

On each machine that renders:

| Requirement | Value |
| --- | --- |
| Operating system | Windows 10 or 11, x64 |
| Houdini | 22.0, the build the package was compiled against or a later 22.0 build |
| GPU | DirectX 12 with hardware ray tracing |
| Runtime libraries | Microsoft Visual C++ 2015-2022 x64 redistributable (Houdini installs it) |

The workstations need no Unreal Engine, Visual Studio or OpenUSD.

On the machine that builds the package, you need the Unreal Engine 5.8.4 source and the toolchain listed in [Building from source](../building.md).

## Building the package

Run these from the root of the hdUE checkout, after a first build as described in [Building from source](../building.md):

```powershell
.\scripts\Build-Backend.ps1 -Configuration Test
.\scripts\Update-HoudiniRuntime.ps1 -Configuration Test -OutputConfig .local\runtime-release.json
.\scripts\Package-Houdini.ps1 -RuntimeConfig .local\runtime-release.json -Zip -UnrealLicenseesOnly
```

1. `Build-Backend.ps1 -Configuration Test` builds Unreal Engine in Epic's Test configuration: Shipping's optimizations, with the console variables that hdUE's `unreal:cvar:*` settings need.
2. `Update-HoudiniRuntime.ps1` stages that build into a runtime profile.
3. `Package-Houdini.ps1` writes the package. `-UnrealLicenseesOnly` confirms the distribution terms above; the script refuses to run without it.

The script also refuses to package when:

- the profile's backend is not the current Test build;
- the checkout has uncommitted changes;
- the Houdini delegate was built from another commit (run `.\scripts\Build-Houdini.ps1` first).

The result is in `out\package\`:

| File | Contents |
| --- | --- |
| `hdUE-<version>-houdini<build>-win64\` | The package folder, about 1.2 GB. For example `hdUE-v0.2.0-houdini22.0.368-win64`. |
| `hdUE-<version>-houdini<build>-win64.zip` | The same folder zipped, about 0.9 GB. |
| `hdUE-<version>-houdini<build>-win64-symbols.zip` | Debug symbols. Keep it with the release to read crash dumps; do not ship it. |

`<version>` is the checkout's `git describe`. The delegate DLL's file properties (Details tab) show the same version and the Houdini build it was compiled for.

## Installing on a workstation

1. Unzip the package folder where it will stay. A shared, read-only network folder works: each user's logs and shader cache go to their own Houdini preferences folder.
2. Close Houdini.
3. Double-click `Install.cmd` in the package folder.
4. Start Houdini.

`Install.cmd` writes one file, `Documents\houdini22.0\packages\hdUE.json` (or under `HOUDINI_USER_PREF_DIR` when you set it). Houdini, hython and husk all read it at startup. It sets the variables listed in [Environment variables](environment-variables.md) and adds the Solaris integration to `HOUDINI_PATH`.

If you move the folder, run `Install.cmd` again.

### Studio-wide install

Write the package file into a shared package folder instead of each user's Documents:

```powershell
.\Install.ps1 -PackagesDirectory \\server\houdini\packages
```

Point every machine's `HOUDINI_PACKAGE_DIR` at that folder.

## Checking that it works

1. In a Solaris Scene Viewer, open the renderer menu. **Unreal Engine** is listed.
2. Pick it. The first time in a session, Unreal Engine starts inside Houdini; the viewport shows the scene a few seconds later.
3. In a terminal, run:

    ```powershell
    husk --renderer "Unreal Engine" -o check.exr "$env:HDUE\samples\cornell\cornell.usda"
    ```

    husk writes `check.exr`, the Cornell box at 512 x 512.

Then follow [Your first render](first-render.md).

## Uninstalling

1. Run `Uninstall.cmd` in the package folder. It deletes `hdUE.json` and nothing else.
2. Delete the package folder.

For a studio install, pass the same folder: `.\Uninstall.ps1 -PackagesDirectory \\server\houdini\packages`.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| **Unreal Engine** is missing from the renderer menu. | Houdini did not load `hdUE.json`: it is missing, or this Houdini is older than the build the package was compiled for. | Run `Install.cmd` again and restart Houdini. Check that your Houdini build is at least the one in the package name. |
| `Incomplete hdUE package: ... is missing.` when you run `Install.cmd`. | The folder was partly copied or unzipped. | Unzip the whole package again. |
| `hdUE warning: hdUE was built against Houdini 22.0.<build> but runs in Houdini 22.0.<other build>` | SideFX guarantees binary compatibility only within one Houdini build. | Nothing to do if renders work. If rendering crashes or misbehaves, build a package for this Houdini build. |
| The viewport stays empty after you pick Unreal Engine. | Unreal Engine failed to start. | Open **Unreal > Open Unreal Engine Logs** and read the newest log. |
