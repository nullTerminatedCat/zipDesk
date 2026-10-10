param(
    [string]$ExePath = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $candidates = @(
        (Join-Path $PSScriptRoot "..\build\ZipDesk.exe"),
        (Join-Path $PSScriptRoot "..\build\Release\ZipDesk.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            $ExePath = (Resolve-Path $candidate).Path
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ExePath) -or -not (Test-Path $ExePath)) {
    throw "Pass -ExePath to ZipDesk.exe"
}

$ExePath = (Resolve-Path $ExePath).Path
$icon = "$ExePath,0"

function Set-DefaultValue([string]$Key, [string]$Value) {
    $created = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($Key)
    $created.SetValue("", $Value)
    $created.Close()
}

function Set-NamedValue([string]$Key, [string]$Name, [string]$Value) {
    $created = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($Key)
    $created.SetValue($Name, $Value)
    $created.Close()
}

function Add-Command([string]$Key, [string]$Label, [string]$Command) {
    Set-DefaultValue $Key $Label
    Set-NamedValue $Key "MUIVerb" $Label
    Set-DefaultValue "$Key\command" $Command
}

function Register-Archive([string]$Extension, [string]$ProgId, [string]$Description) {
    $open = "`"$ExePath`" `"%1`""
    $here = "`"$ExePath`" --extract-here `"%1`""
    $folder = "`"$ExePath`" --extract-to-folder `"%1`""
    Set-DefaultValue "Software\Classes\$Extension" $ProgId
    Set-DefaultValue "Software\Classes\$ProgId" $Description
    Set-DefaultValue "Software\Classes\$ProgId\DefaultIcon" $icon
    Set-DefaultValue "Software\Classes\$ProgId\shell\open" "用日常解压打开"
    Set-NamedValue "Software\Classes\$ProgId\shell\open" "Icon" $icon
    Set-DefaultValue "Software\Classes\$ProgId\shell\open\command" $open

    $menu = "Software\Classes\SystemFileAssociations\$Extension\shell\ZipDesk"
    Set-DefaultValue $menu "日常解压"
    Set-NamedValue $menu "MUIVerb" "日常解压"
    Set-NamedValue $menu "Icon" $icon
    Set-NamedValue $menu "SubCommands" ""
    Set-NamedValue $menu "Position" "Top"
    Add-Command "$menu\shell\a_here" "解压到当前目录" $here
    Add-Command "$menu\shell\b_folder" "解压到同名文件夹" $folder
}

function Register-Compress([string]$Parent) {
    $zip = "`"$ExePath`" --compress-zip `"%1`""
    $seven = "`"$ExePath`" --compress-7z `"%1`""
    $menu = "$Parent\shell\ZipDesk"
    Set-DefaultValue $menu "日常解压"
    Set-NamedValue $menu "MUIVerb" "日常解压"
    Set-NamedValue $menu "Icon" $icon
    Set-NamedValue $menu "SubCommands" ""
    Add-Command "$menu\shell\a_zip" "压缩为 zip" $zip
    Add-Command "$menu\shell\b_7z" "压缩为 7z" $seven
}

Register-Archive ".zip" "ZipDesk.zip" "ZIP 压缩文件"
Register-Archive ".7z" "ZipDesk.7z" "7z 压缩文件"
Register-Compress "Software\Classes\*"
Register-Compress "Software\Classes\Directory"
Write-Output "Registered $ExePath"
