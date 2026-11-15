# Assembles the downloadable LSD_Engine folder from a Release build and zips it.
# The folder is also what packaging/LSD_Engine.iss installs, so this file is the one list of what ships.
#   powershell -ExecutionPolicy Bypass -File packaging/make_portable.ps1
# Output: build/package/LSD_Engine/ and build/package/LSD_Engine_Windows.zip
param(
    [string]$BuildDir = "build/windows-msvc/editor/Release",
    [string]$OutDir = "build/package"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$dest = Join-Path $OutDir "LSD_Engine"
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force $dest | Out-Null

# The editor (also the game) and the Claude MCP bridge it starts from its own folder.
foreach ($exe in "lsd_editor.exe", "lsd_mcp.exe") {
    $src = Join-Path $BuildDir $exe
    if (-not (Test-Path $src)) { throw "$src is missing: build Release first (cmake --build --preset windows-msvc --config Release)" }
    Copy-Item $src $dest
}
# The Visual C++ runtime beside the exe (app-local, like File > Package Project), for PCs without the
# redistributable: from Visual Studio's redist folder when the build environment names it, else System32.
$crt = Get-ChildItem -Directory "$env:VCToolsRedistDir\x64\Microsoft.VC*.CRT" -ErrorAction SilentlyContinue | Select-Object -First 1
$crtDir = if ($env:VCToolsRedistDir -and $crt) { $crt.FullName } else { "$env:SystemRoot\System32" }
foreach ($dll in "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll") {
    Copy-Item (Join-Path $crtDir $dll) $dest
}

# The editor works in its own folder: the sample project (Config, Content, Source), the headers C / C++
# game code compiles against, the script API the LSD Assistant reads, the licences and the docs.
$copy = @(
    "Config", "Content", "Source", "docs",
    "engine/game_api/include",
    "engine/script/include/lsd/script/script.h",
    "LICENSE", "THIRD-PARTY-NOTICES.md", "README.md"
)
foreach ($item in $copy) {
    if (-not (Test-Path $item)) { throw "$item is missing" }
    $target = Join-Path $dest $item
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -Recurse $item $target
}
# Per-user editor state never ships (a source tree that has been opened in the editor has some).
Get-ChildItem -Recurse -Directory $dest -Include DerivedData, Saved, Intermediate, Binaries, .obsidian |
    Remove-Item -Recurse -Force

$zip = Join-Path $OutDir "LSD_Engine_Windows.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $dest -DestinationPath $zip
$files = (Get-ChildItem -Recurse -File $dest).Count
"{0}: {1} files, {2:N1} MB" -f $zip, $files, ((Get-Item $zip).Length / 1MB)
