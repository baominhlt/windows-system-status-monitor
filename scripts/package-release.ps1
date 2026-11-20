# Builds the exe and packs the release assets into dist\:
#   WindowsSystemStatusMonitor.exe                      (the executable on its own, for a direct download)
#   WindowsSystemStatusMonitor-v<VERSION>-win-x64.zip   (exe + README files + LICENSE, inside one top-level folder)
#   SHA256SUMS.txt                                      (SHA-256 of both files above)
# Usage (from anywhere):  powershell -ExecutionPolicy Bypass -File scripts\package-release.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = (Get-Content VERSION -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must look like 1.2.3, got '$version'" }

& cmd /c "`"$root\scripts\build.bat`""
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$name = "WindowsSystemStatusMonitor-v$version-win-x64"
Remove-Item dist -Recurse -Force -ErrorAction SilentlyContinue
$stage = Join-Path $root "dist\$name"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item build\WindowsSystemStatusMonitor.exe, README.md, README.vi.md, LICENSE $stage

# tar.exe (bundled with Windows 10+) writes portable zip entries with forward slashes.
& tar.exe -a -c -f "dist\$name.zip" -C dist $name
if ($LASTEXITCODE -ne 0) { throw 'zip failed' }
Remove-Item $stage -Recurse -Force

# The executable is also published on its own, so it can be downloaded and run without unpacking anything.
Copy-Item build\WindowsSystemStatusMonitor.exe dist\

$sums = foreach ($f in 'WindowsSystemStatusMonitor.exe', "$name.zip") {
    $h = (Get-FileHash "dist\$f" -Algorithm SHA256).Hash.ToLower()
    "$h  $f"
}
$sums | Set-Content dist\SHA256SUMS.txt -Encoding ascii
Write-Host "Release assets in dist\:"
Get-ChildItem dist | Select-Object Name, Length | Format-Table -AutoSize
