<#
.SYNOPSIS
  Generates a CycloneDX 1.5 and an SPDX 2.3 SBOM (JSON) for a built release executable.

.DESCRIPTION
  The project has no package manager, so a generic scanner finds nothing. Instead everything is read from the
  shipped executable itself: its PE import table (system DLLs), its linker version (MSVC toolset), whether the
  C runtime is statically linked, and signatures of components it loads at run time (NVML, UI Automation,
  icon fonts). Nothing is taken from the machine that runs this script, and no local paths are written.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\generate-sbom.ps1 `
      -Exe build\WindowsSystemStatusMonitor.exe -Zip dist\WindowsSystemStatusMonitor-v1.0.0-win-x64.zip -Version 1.0.0
#>
param(
    [Parameter(Mandatory)] [string]$Exe,          # the executable that was released
    [Parameter(Mandatory)] [string]$Zip,          # the released zip (its hash and URL go into the SBOM)
    [Parameter(Mandatory)] [string]$Version,      # e.g. 1.0.0 (no leading v)
    [string]$Repo = $(if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { 'windows-system-status-monitor' }),
    [string]$OutDir = 'dist'
)
$ErrorActionPreference = 'Stop'
$ToolVersion = '1.0.0'

if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 1.2.3, got '$Version'." }
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "Executable not found: $Exe" }
if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { throw "Zip not found: $Zip" }

$AppName = 'Windows System Status Monitor'
$Base = "WindowsSystemStatusMonitor-v$Version-win-x64"
$Copyright = 'Copyright (c) 2026 Windows System Status Monitor contributors'
$HasRepo = $Repo -match '^[^/\s]+/[^/\s]+$'
$RepoUrl = if ($HasRepo) { "https://github.com/$Repo" } else { $null }
$Purl = if ($HasRepo) { "pkg:github/$Repo@v$Version" } else { $null }
$ZipUrl = if ($HasRepo) { "https://github.com/$Repo/releases/download/v$Version/$(Split-Path -Leaf $Zip)" } else { 'NOASSERTION' }

# ---- read the executable ---------------------------------------------------------------------------------
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Exe).Path)

function Read-PeImports([byte[]]$b) {
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { throw 'Not a PE file (no MZ header).' }
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ($pe -lt 0 -or $pe + 24 -gt $b.Length -or [BitConverter]::ToUInt32($b, $pe) -ne 0x00004550) { throw 'Not a PE file (no PE signature).' }
    $sections = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24
    if ([BitConverter]::ToUInt16($b, $opt) -ne 0x20B) { throw 'Only 64-bit (PE32+) executables are supported.' }
    $linker = '{0}.{1}' -f $b[$opt + 2], $b[$opt + 3]
    $importRva = [BitConverter]::ToUInt32($b, $opt + 120)  # data directory 1 = import table

    $secTable = $opt + $optSize
    $toOffset = {
        param([uint32]$rva)
        for ($i = 0; $i -lt $sections; $i++) {
            $s = $secTable + 40 * $i
            $va = [BitConverter]::ToUInt32($b, $s + 12)
            $size = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
            if ($rva -ge $va -and $rva -lt $va + $size) { return [int]($rva - $va + [BitConverter]::ToUInt32($b, $s + 20)) }
        }
        throw "RVA 0x$($rva.ToString('X')) is outside every section."
    }

    $dlls = New-Object System.Collections.Generic.SortedSet[string]
    if ($importRva -ne 0) {
        $d = & $toOffset $importRva
        while ($d + 20 -le $b.Length) {
            $nameRva = [BitConverter]::ToUInt32($b, $d + 12)
            if ($nameRva -eq 0) { break }  # end of the descriptor table
            $n = & $toOffset $nameRva
            $e = $n
            while ($b[$e] -ne 0) { $e++ }
            [void]$dlls.Add([Text.Encoding]::ASCII.GetString($b, $n, $e - $n).ToLowerInvariant())
            $d += 20
        }
    }
    [pscustomobject]@{ Linker = $linker; Dlls = @($dlls) }
}

$pe = Read-PeImports $bytes
$latin1 = [Text.Encoding]::GetEncoding(28591).GetString($bytes)   # 1 char per byte: cheap substring search
function Has-Ascii([string]$s) { $latin1.IndexOf($s, [StringComparison]::Ordinal) -ge 0 }
function Has-Utf16([string]$s) { Has-Ascii (($s.ToCharArray() | ForEach-Object { "$_`0" }) -join '') }
function Has-Bytes([byte[]]$needle) { Has-Ascii ([Text.Encoding]::GetEncoding(28591).GetString($needle)) }

$exeSha256 = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash.ToLowerInvariant()
$exeSha1 = (Get-FileHash -LiteralPath $Exe -Algorithm SHA1).Hash.ToLowerInvariant()
$zipSha256 = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant()

# ---- dependency model ---------------------------------------------------------------------------------------
# kind: static = linked into the exe, required = needed at run time, optional = used when present
$deps = New-Object System.Collections.ArrayList
function Add-Dep($id, $name, $version, $kind, $type, $supplier, $description, $license) {
    [void]$deps.Add([pscustomobject]@{
        Id = $id; Name = $name; Version = $version; Kind = $kind; Type = $type
        Supplier = $supplier; Description = $description; License = $license
    })
}

$crtPattern = '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt|msvcr)'
$crtDlls = @($pe.Dlls | Where-Object { $_ -match $crtPattern })
$staticCrt = $crtDlls.Count -eq 0
$msLicense = 'Microsoft Visual C++ Redistributable license terms'
if ($staticCrt) {
    Add-Dep 'msvc-crt-static' 'Microsoft Visual C++ runtime (static, libcmt)' $pe.Linker 'static' 'library' 'Microsoft Corporation' `
        "C runtime statically linked into the executable; part of MSVC toolset $($pe.Linker). Nothing is installed or redistributed separately." $msLicense
} else {
    foreach ($d in $crtDlls) {
        Add-Dep "dll-$d" $d $null 'required' 'library' 'Microsoft Corporation' `
            'Visual C++ / Universal C runtime library imported dynamically. Install the Visual C++ Redistributable.' $msLicense
    }
}
foreach ($d in ($pe.Dlls | Where-Object { $_ -notmatch $crtPattern })) {
    Add-Dep "dll-$d" $d $null 'required' 'library' 'Microsoft Corporation' `
        'Windows system library supplied by the operating system; not redistributed with the application. Version depends on the Windows build.' $null
}

# Components loaded at run time are not in the import table: detect them from signatures inside the executable.
# UI Automation client: CLSID_CUIAutomation {ff48dba4-60ef-4201-aa87-54103eef594e} / IID_IUIAutomation {30cbe57d-d9d0-452a-ab13-7ac5ac4825ee}
$clsidUia = [byte[]](0xA4, 0xDB, 0x48, 0xFF, 0xEF, 0x60, 0x01, 0x42, 0xAA, 0x87, 0x54, 0x10, 0x3E, 0xEF, 0x59, 0x4E)
$iidUia = [byte[]](0x7D, 0xE5, 0xCB, 0x30, 0xD0, 0xD9, 0x2A, 0x45, 0xAB, 0x13, 0x7A, 0xC5, 0xAC, 0x48, 0x25, 0xEE)
if ((Has-Bytes $clsidUia) -or (Has-Bytes $iidUia)) {
    Add-Dep 'dll-uiautomationcore.dll' 'UIAutomationCore.dll' $null 'required' 'library' 'Microsoft Corporation' `
        'Microsoft UI Automation client library, created through COM (CUIAutomation) at run time to measure taskbar buttons. Supplied by the operating system.' $null
}
if ((Has-Ascii 'nvmlInit_v2') -or (Has-Utf16 'nvml.dll')) {
    Add-Dep 'nvml' 'NVIDIA Management Library (nvml.dll)' $null 'optional' 'library' 'NVIDIA Corporation' `
        'Optional. Loaded at run time with LoadLibrary from the installed NVIDIA driver to read GPU usage, memory and temperature; not distributed with the application. Disabled by nvidia_polling=0.' $null
}
foreach ($f in 'Segoe Fluent Icons', 'Segoe MDL2 Assets') {
    if (Has-Utf16 $f) {
        Add-Dep ('font-' + ($f -replace '\s', '-').ToLowerInvariant()) $f $null 'optional' 'data' 'Microsoft Corporation' `
            'Optional. Operating-system icon font used for the metric icons; the application falls back to text labels when it is missing. Not distributed.' $null
    }
}

$now = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$guid = [guid]::NewGuid().ToString()
$utf8 = New-Object Text.UTF8Encoding($false)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path   # .NET file APIs resolve relative paths against the process directory, not the PowerShell location

# ---- CycloneDX 1.5 -------------------------------------------------------------------------------------------
$cdxComponents = @(foreach ($d in $deps) {
    $c = [ordered]@{
        type = $d.Type; 'bom-ref' = $d.Id; name = $d.Name
        scope = $(if ($d.Kind -eq 'optional') { 'optional' } else { 'required' })
        supplier = [ordered]@{ name = $d.Supplier }
        description = $d.Description
    }
    if ($d.Version) { $c.version = $d.Version }
    if ($d.License) { $c.licenses = @([ordered]@{ license = [ordered]@{ name = $d.License } }) }
    $c.properties = @([ordered]@{ name = 'dependency-kind'; value = $d.Kind })
    $c
})
$app = [ordered]@{
    type = 'application'; 'bom-ref' = 'app'; name = $AppName; version = $Version
    description = 'Status strip on the Windows taskbar showing CPU, RAM, GPU, disk, network and temperatures.'
    licenses = @([ordered]@{ license = [ordered]@{ id = 'MIT' } })
    publisher = 'Windows System Status Monitor contributors'
    hashes = @([ordered]@{ alg = 'SHA-256'; content = $exeSha256 }, [ordered]@{ alg = 'SHA-1'; content = $exeSha1 })
    properties = @(
        [ordered]@{ name = 'executable'; value = (Split-Path -Leaf $Exe) }
        [ordered]@{ name = 'release-zip:sha256'; value = $zipSha256 }
        [ordered]@{ name = 'msvc-linker-version'; value = $pe.Linker }
    )
}
if ($HasRepo) {
    $app.purl = $Purl
    $app.externalReferences = @([ordered]@{ type = 'website'; url = $RepoUrl }, [ordered]@{ type = 'vcs'; url = $RepoUrl })
}
$cdx = [ordered]@{
    bomFormat = 'CycloneDX'; specVersion = '1.5'; serialNumber = "urn:uuid:$guid"; version = 1
    metadata = [ordered]@{
        timestamp = $now
        tools = [ordered]@{ components = @([ordered]@{ type = 'application'; name = 'generate-sbom.ps1'; version = $ToolVersion }) }
        component = $app
    }
    components = $cdxComponents
    dependencies = @(
        [ordered]@{ ref = 'app'; dependsOn = @($deps | ForEach-Object { $_.Id }) }
        foreach ($d in $deps) { [ordered]@{ ref = $d.Id; dependsOn = @() } }
    )
}
$cdxPath = Join-Path $OutDir "$Base.cdx.json"
[IO.File]::WriteAllText($cdxPath, ((ConvertTo-Json -InputObject $cdx -Depth 12) + "`n"), $utf8)

# ---- SPDX 2.3 --------------------------------------------------------------------------------------------------
function Spdx-Id([string]$s) { 'SPDXRef-Package-' + ($s -replace '[^A-Za-z0-9.-]', '-') }
$appId = 'SPDXRef-Package-app'
$spdxApp = [ordered]@{
    name = $AppName; SPDXID = $appId; versionInfo = $Version
    supplier = 'Organization: Windows System Status Monitor contributors'
    downloadLocation = $ZipUrl; filesAnalyzed = $false
    checksums = @([ordered]@{ algorithm = 'SHA256'; checksumValue = $zipSha256 })
    licenseConcluded = 'MIT'; licenseDeclared = 'MIT'; copyrightText = $Copyright
    primaryPackagePurpose = 'APPLICATION'
    comment = "Checksums above are for the release zip. The executable $(Split-Path -Leaf $Exe) has SHA-256 $exeSha256 and SHA-1 $exeSha1; MSVC linker version $($pe.Linker)."
}
if ($HasRepo) {
    $spdxApp.homepage = $RepoUrl
    $spdxApp.externalRefs = @([ordered]@{ referenceCategory = 'PACKAGE-MANAGER'; referenceType = 'purl'; referenceLocator = $Purl })
}
$spdxPackages = @($spdxApp) + @(foreach ($d in $deps) {
    $p = [ordered]@{
        name = $d.Name; SPDXID = (Spdx-Id $d.Id)
        supplier = "Organization: $($d.Supplier)"
        downloadLocation = 'NOASSERTION'; filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'; licenseDeclared = 'NOASSERTION'; copyrightText = 'NOASSERTION'
        primaryPackagePurpose = $(if ($d.Type -eq 'data') { 'OTHER' } else { 'LIBRARY' })
        description = $d.Description
    }
    if ($d.Version) { $p.versionInfo = $d.Version }
    $p
})
$rel = New-Object System.Collections.ArrayList
[void]$rel.Add([ordered]@{ spdxElementId = 'SPDXRef-DOCUMENT'; relationshipType = 'DESCRIBES'; relatedSpdxElement = $appId })
foreach ($d in $deps) {
    $id = Spdx-Id $d.Id
    switch ($d.Kind) {
        'static'   { [void]$rel.Add([ordered]@{ spdxElementId = $appId; relationshipType = 'STATIC_LINK'; relatedSpdxElement = $id }) }
        'optional' { [void]$rel.Add([ordered]@{ spdxElementId = $id; relationshipType = 'OPTIONAL_DEPENDENCY_OF'; relatedSpdxElement = $appId }) }
        default    { [void]$rel.Add([ordered]@{ spdxElementId = $appId; relationshipType = 'DEPENDS_ON'; relatedSpdxElement = $id }) }
    }
}
$spdx = [ordered]@{
    spdxVersion = 'SPDX-2.3'; dataLicense = 'CC0-1.0'; SPDXID = 'SPDXRef-DOCUMENT'
    name = "$Base"
    documentNamespace = $(if ($HasRepo) { "https://github.com/$Repo/releases/download/v$Version/$Base-$guid" } else { "https://spdx.org/spdxdocs/$Base-$guid" })
    creationInfo = [ordered]@{ created = $now; creators = @("Tool: generate-sbom.ps1-$ToolVersion") }
    packages = $spdxPackages
    relationships = @($rel)
}
$spdxPath = Join-Path $OutDir "$Base.spdx.json"
[IO.File]::WriteAllText($spdxPath, ((ConvertTo-Json -InputObject $spdx -Depth 12) + "`n"), $utf8)

Write-Host "SBOM: $($deps.Count) components (static CRT: $staticCrt, MSVC linker $($pe.Linker), $($pe.Dlls.Count) imported DLLs)"
Write-Host "  $cdxPath"
Write-Host "  $spdxPath"
