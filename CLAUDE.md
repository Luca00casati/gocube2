# GoCube: notes for Claude

## Build and test
Commands need the Visual Studio developer environment (MSVC, Ninja, `VCPKG_ROOT`).
From PowerShell outside it:

    $vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"

then `cmake --preset windows`, `cmake --build --preset debug`, `ctest --preset debug`.

## Graphics
Direct3D 11, feature level 11_0 minimum. Not Direct3D 12: the development PC's
Intel HD Graphics 4600 hangs on every Direct3D 12 draw (Intel disabled Direct3D 12
on Haswell). Check rendering changes on that GPU, not only on WARP:
`build\Debug\gocube.exe --hidden --frames=600` must exit 0 (debug layer on).

## Plan
The rewrite follows the GoCube Native Rewrite Plan
(https://claude.ai/artifact/CMdng1jnMGWsAb7X84FHoy); the Godot version in
`../gocube` is the reference for behaviour.
