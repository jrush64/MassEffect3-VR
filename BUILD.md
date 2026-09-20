# Building MELE3 VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. `build.bat` locates it
  automatically via `vswhere.exe`. The project does not pin a Windows SDK version; whichever
  is installed is used.
- No other tools or package managers required. MinHook and Dear ImGui are vendored in
  `third_party/` - nothing to fetch or install separately.

## Build

```
scripts\build.bat
```

Output: `builds\dxgi.dll`.

The build runs MSBuild on `src\MELE3VR.vcxproj` (Release|x64) with the mod's own sources
plus the vendored MinHook and Dear ImGui sources, linked against the dynamic MSVC runtime, so the machine running the mod needs the
Microsoft Visual C++ 2015-2022 x64 Redistributable (most systems already have it). You can also open the project in
Visual Studio directly.

## Deploying

Copy these files into `<Steam library>\steamapps\common\Mass Effect Legendary Edition\Game\ME3\Binaries\Win64\`,
next to `MassEffect3.exe`:

- `builds\dxgi.dll` (just built)
- `openxr_loader.dll` - an x64 build of the official [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK), not built by this repo (a copy is in `release/`)
- `MELE3VR.ini` (see `runtime/MELE3VR.ini` for a starting point)

See `release/README.txt` for the installer script and end-user install steps.

## Third-party components

| Component | Location | License |
|---|---|---|
| MinHook | `third_party/minhook` | BSD 2-Clause (see its `LICENSE.txt`) |
| Dear ImGui | `third_party/imgui` | MIT (see its `LICENSE.txt`) |
| OpenXR loader | `release/openxr_loader.dll`, obtained separately | Apache 2.0 |
