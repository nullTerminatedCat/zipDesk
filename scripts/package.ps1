param(
    [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
    [string]$Iscc = "tools/innosetup/ISCC.exe",
    [ValidateSet('x64', 'x86')][string]$Architecture = 'x64',
    [string]$BuildDirectory = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$version = '0.2.14'
$name = "QingXia-$version-windows-$Architecture"
if (!$BuildDirectory) { $BuildDirectory = if ($Architecture -eq 'x86') { 'build-x86' } else { 'build' } }
$archive = Join-Path $root "dist/$name-portable.zip"
$installer = Join-Path $root "dist/$name-setup.exe"
foreach ($asset in @($archive, $installer)) {
    if (Test-Path $asset) { throw "Release asset already exists: $asset" }
}
$stage = Join-Path $root ("dist/staging-" + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage $name
New-Item -ItemType Directory -Path $payload -Force | Out-Null

# Explicit allowlist: never distribute the build tree or test executables.
$files = @('ZipDesk.exe', '7z.dll', '7z.sfx', 'Qt6Core.dll', 'Qt6Gui.dll',
    'Qt6Network.dll', 'Qt6Widgets.dll')
foreach ($file in $files) { Copy-Item (Join-Path $BuildDirectory $file) $payload }
foreach ($file in @('d3dcompiler_47.dll', 'dxcompiler.dll', 'dxil.dll')) {
    $source = Join-Path $BuildDirectory $file
    if (Test-Path $source) { Copy-Item $source $payload }
}
foreach ($folder in @('generic', 'imageformats', 'networkinformation', 'platforms', 'styles', 'tls')) {
    $source = Join-Path $BuildDirectory $folder
    if (Test-Path $source) { Copy-Item $source $payload -Recurse }
}
if (!(Test-Path (Join-Path $payload 'platforms/qwindows.dll'))) { throw 'Missing Qt Windows platform plugin' }
$runtimeFiles = @('msvcp140.dll', 'vcruntime140.dll')
if ($Architecture -eq 'x64') { $runtimeFiles += 'vcruntime140_1.dll' }
foreach ($required in $runtimeFiles) {
    if (!(Test-Path (Join-Path $RuntimeDirectory $required))) { throw "Missing runtime: $required" }
}
Copy-Item (Join-Path $RuntimeDirectory '*.dll') $payload
# Reject accidentally mixed x86/x64 dependencies before producing an installer.
$expectedMachine = if ($Architecture -eq 'x86') { 0x014c } else { 0x8664 }
foreach ($binary in (Get-ChildItem $payload -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll', '.sfx' })) {
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($binary.FullName))
    try {
        $reader.BaseStream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        $reader.BaseStream.Position = $peOffset
        $signature = $reader.ReadUInt32()
        $machine = $reader.ReadUInt16()
        # Official 7-Zip may ship an x86 SFX stub even in its x64 distribution.
        # It runs as a separate extractor, unlike DLLs loaded by the application.
        $compatibleSfx = $Architecture -eq 'x64' -and $binary.Extension -eq '.sfx' -and $machine -eq 0x014c
        if ($signature -ne 0x00004550 -or ($machine -ne $expectedMachine -and !$compatibleSfx)) {
            throw "Architecture mismatch: $($binary.FullName)"
        }
    } finally { $reader.Dispose() }
}
Copy-Item docs/licenses (Join-Path $payload 'licenses') -Recurse
Copy-Item THIRD_PARTY_NOTICES.md $payload
Copy-Item LICENSE $payload
Copy-Item docs/PORTABLE.txt $payload
$revision = (git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source revision' }
$dirty = [bool](git status --porcelain --untracked-files=normal)
"Version: $version`r`nArchitecture: $Architecture`r`nSource revision: $revision`r`nWorking tree modified: $dirty" | Set-Content (Join-Path $payload 'BUILD.txt') -Encoding UTF8

if (!(Test-Path $Iscc)) { throw "Inno Setup compiler not found: $Iscc" }
& $Iscc "/DAppSourceDir=$payload" "/DAppArchitecture=$Architecture" installer/ZipDesk.iss
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed: $LASTEXITCODE" }
Compress-Archive -Path $payload -DestinationPath $archive -CompressionLevel Optimal
$assets = @((Join-Path $root "dist/$name-setup.exe"), $archive)
$hashes = foreach ($asset in $assets) {
    $hash = (Get-FileHash $asset -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($asset))"
}
$checksumName = "SHA256SUMS-$version-$Architecture.txt"
$hashes | Set-Content (Join-Path $root "dist/$checksumName") -Encoding ASCII
Write-Output "Payload: $payload"
Get-Item $assets | Select-Object Name, Length
