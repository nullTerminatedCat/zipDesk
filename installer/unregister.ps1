$ErrorActionPreference = "Stop"

function Remove-Key([string]$Key) {
    try {
        [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($Key, $false)
    } catch {
    }
}

foreach ($extension in @(".zip", ".7z")) {
    $item = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Classes\$extension")
    if ($null -ne $item) {
        $defaultValue = [string]$item.GetValue("")
        $item.Close()
        $expected = if ($extension -eq ".zip") { "ZipDesk.zip" } else { "ZipDesk.7z" }
        if ($defaultValue -eq $expected) {
            Remove-Key "Software\Classes\$extension"
        }
    }
    Remove-Key "Software\Classes\SystemFileAssociations\$extension\shell\ZipDesk"
    Remove-Key "Software\Classes\SystemFileAssociations\$extension\shell\ZipDesk.ExtractHere"
    Remove-Key "Software\Classes\SystemFileAssociations\$extension\shell\ZipDesk.ExtractToFolder"
}

Remove-Key "Software\Classes\ZipDesk.zip"
Remove-Key "Software\Classes\ZipDesk.7z"
Remove-Key "Software\Classes\*\shell\ZipDesk"
Remove-Key "Software\Classes\*\shell\ZipDesk.CompressZip"
Remove-Key "Software\Classes\*\shell\ZipDesk.Compress7z"
Remove-Key "Software\Classes\Directory\shell\ZipDesk"
Remove-Key "Software\Classes\Directory\shell\ZipDesk.CompressZip"
Remove-Key "Software\Classes\Directory\shell\ZipDesk.Compress7z"
Write-Output "Unregistered ZipDesk"
