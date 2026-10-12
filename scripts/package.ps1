param(
    [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
    [string]$Iscc = "tools/innosetup/ISCC.exe"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$version = '0.1.0'
$name = "QingXia-$version-windows-x64"
$stage = Join-Path $root ("dist/staging-" + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage $name
New-Item -ItemType Directory -Path $payload -Force | Out-Null

# Explicit allowlist: never distribute the build tree or test executables.
$files = @('ZipDesk.exe', '7z.dll', '7z.sfx', 'Qt6Core.dll', 'Qt6Gui.dll',
    'Qt6Network.dll', 'Qt6Widgets.dll', 'd3dcompiler_47.dll', 'dxcompiler.dll', 'dxil.dll')
foreach ($file in $files) { Copy-Item (Join-Path 'build' $file) $payload }
foreach ($folder in @('generic', 'imageformats', 'networkinformation', 'platforms', 'styles', 'tls')) {
    Copy-Item (Join-Path 'build' $folder) $payload -Recurse
}
foreach ($required in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    if (!(Test-Path (Join-Path $RuntimeDirectory $required))) { throw "Missing runtime: $required" }
}
Copy-Item (Join-Path $RuntimeDirectory '*.dll') $payload
Copy-Item docs/licenses (Join-Path $payload 'licenses') -Recurse
Copy-Item THIRD_PARTY_NOTICES.md $payload
Copy-Item LICENSE $payload
Copy-Item docs/PORTABLE.txt $payload
$revision = (git rev-parse HEAD).Trim()
"Version: $version`r`nSource revision: $revision" | Set-Content (Join-Path $payload 'BUILD.txt') -Encoding UTF8

if (!(Test-Path $Iscc)) { throw "Inno Setup compiler not found: $Iscc" }
& $Iscc "/DAppSourceDir=$payload" installer/ZipDesk.iss
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed: $LASTEXITCODE" }
$archive = Join-Path $root "dist/$name-portable.zip"
if (Test-Path $archive) { throw "Release archive already exists: $archive" }
Compress-Archive -Path $payload -DestinationPath $archive -CompressionLevel Optimal
$assets = @((Join-Path $root "dist/$name-setup.exe"), $archive)
$hashes = foreach ($asset in $assets) {
    $hash = (Get-FileHash $asset -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($asset))"
}
$hashes | Set-Content (Join-Path $root 'dist/SHA256SUMS.txt') -Encoding ASCII
Write-Output "Payload: $payload"
Get-Item $assets | Select-Object Name, Length
