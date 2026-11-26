[CmdletBinding()]
param(
    [ValidateSet('Verify','Inspect','Upgrade','FullInstall')][string]$Mode = 'Verify',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Serial,
    [switch]$FormatUserdataExt4,
    [string]$PackageRoot,
    [string]$FastbootPath
)

$ErrorActionPreference = 'Stop'

if ($FormatUserdataExt4 -and $Mode -ne 'FullInstall') {
    throw '-FormatUserdataExt4 can only be used together with -Mode FullInstall.'
}

if (!$PackageRoot) {
    $PackageRoot = [IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)
}
$PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path

$ManifestPath = Join-Path $PackageRoot 'PACKAGE.json'
if (!(Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw 'PACKAGE.json is missing.'
}
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json

if ($Manifest.kind -ne 'marble-anyhmos-fastboot' -or $Manifest.device -ne 'marble' -or $Manifest.target_slot -ne 'a') {
    throw 'Unexpected package identity: expected marble-anyhmos-fastboot (slot a).'
}
if ($Manifest.package_type -notin @('upgrade','full')) {
    throw "Unsupported package_type: $($Manifest.package_type)"
}
if ($Manifest.device_boot_tested -isnot [bool]) {
    throw 'Invalid device_boot_tested field in PACKAGE.json.'
}
if ($Mode -eq 'FullInstall' -and $Manifest.package_type -ne 'full') {
    throw 'This is an upgrade-only package (boot + vendor_boot). Use -Mode Upgrade, or use the full package for FullInstall.'
}

$RequiredImages = @('boot.img','vendor_boot.img')
if ($Manifest.package_type -eq 'full') {
    $RequiredImages += @('vbmeta.img','vbmeta_system.img','super.img')
}
foreach ($Name in $RequiredImages) {
    $Records = @($Manifest.files | Where-Object { $_.path -eq "images/$Name" })
    if ($Records.Count -ne 1) {
        throw "Missing or duplicate artifact in PACKAGE.json: images/$Name"
    }
}

Write-Host '============================================================'
Write-Host "AnyHMOS Xiaomi marble Fastboot Tool ($($Manifest.batch) / $($Manifest.package_type))"
Write-Host "Donor:  $($Manifest.donor)"
Write-Host "Kernel: $($Manifest.kernel_release)"
Write-Host '============================================================'
Write-Host 'Verifying package file sizes and SHA-256 checksums...'

foreach ($Record in $Manifest.files) {
    if ($Record.path -match '(^[\\/]|:|(^|[\\/])\.\.([\\/]|$))') {
        throw "Invalid manifest path: $($Record.path)"
    }
    $RelativeWin = $Record.path -replace '/', '\'
    $FullPath = Join-Path $PackageRoot $RelativeWin
    if (!(Test-Path -LiteralPath $FullPath -PathType Leaf)) {
        throw "Missing file: $($Record.path)"
    }
    $ActualBytes = (Get-Item -LiteralPath $FullPath).Length
    if ($ActualBytes -ne [long]$Record.bytes) {
        throw "Size mismatch for $($Record.path): expected $($Record.bytes), got $ActualBytes"
    }
    $ActualHash = (Get-FileHash -LiteralPath $FullPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($ActualHash -ne $Record.sha256.ToLowerInvariant()) {
        throw "SHA-256 mismatch for $($Record.path)"
    }
    Write-Host "  [OK] $($Record.path) ($ActualBytes bytes)"
}

if ($Manifest.device_boot_tested) {
    Write-Host "Device boot validation: PASSED (boot_id: $($Manifest.verified_boot_id))" -ForegroundColor Green
} else {
    Write-Host 'Device boot validation: NOT marked tested in this package.' -ForegroundColor Yellow
}

if ($Mode -eq 'Verify') {
    Write-Host 'Offline verification passed. No device command was sent.' -ForegroundColor Green
    return
}

if ($FastbootPath) {
    $Fastboot = (Resolve-Path -LiteralPath $FastbootPath).Path
} else {
    $BundledFastboot = Join-Path (Join-Path $PackageRoot 'bin') 'fastboot.exe'
    if (Test-Path -LiteralPath $BundledFastboot -PathType Leaf) {
        $Fastboot = $BundledFastboot
    } else {
        $Cmd = Get-Command fastboot.exe -ErrorAction SilentlyContinue
        if (!$Cmd) { $Cmd = Get-Command fastboot -ErrorAction SilentlyContinue }
        if (!$Cmd) { throw 'fastboot.exe not found in package bin\ or PATH.' }
        $Fastboot = $Cmd.Source
    }
}

$LogDir = Join-Path $PackageRoot 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogPath = Join-Path $LogDir ('fastboot-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')

function Invoke-Fastboot([string[]]$Arguments) {
    foreach ($Argument in $Arguments) {
        if ($Argument.Contains('"') -or $Argument.EndsWith('\')) {
            throw "Unsupported native argument: $Argument"
        }
    }
    $Start = New-Object System.Diagnostics.ProcessStartInfo
    $Start.FileName = $Fastboot
    $Start.WorkingDirectory = Split-Path -Parent $Fastboot
    $Start.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $Start.UseShellExecute = $false
    $Start.CreateNoWindow = $true
    $Start.RedirectStandardOutput = $true
    $Start.RedirectStandardError = $true
    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo = $Start
    try {
        [void]$Process.Start()
        $Stdout = $Process.StandardOutput.ReadToEndAsync()
        $Stderr = $Process.StandardError.ReadToEndAsync()
        $Timeout = 30000
        if (($Arguments -contains 'flash') -or ($Arguments -contains 'erase') -or ($Arguments | Where-Object { $_ -like 'format*' })) {
            $Timeout = 1800000
        }
        if (!$Process.WaitForExit($Timeout)) {
            $Process.Kill()
            throw 'fastboot timed out. Check the USB cable/connection before retrying.'
        }
        $Text = $Stdout.Result + "`n" + $Stderr.Result
        Add-Content -LiteralPath $LogPath -Value ("fastboot " + ($Arguments -join ' ') + "`n" + $Text + "`n")
        if ($Process.ExitCode -ne 0) {
            throw "fastboot failed (exit $($Process.ExitCode)):`n$Text"
        }
        return $Text
    } finally {
        $Process.Dispose()
    }
}

$Devices = Invoke-Fastboot @('devices')
$Found = @([regex]::Matches($Devices, '(?m)^([a-zA-Z0-9_-]+)\s+fastboot\s*$') | ForEach-Object { $_.Groups[1].Value })
if ($Serial) {
    if ($Found -notcontains $Serial) {
        throw "Requested Fastboot serial '$Serial' is not connected."
    }
} else {
    if ($Found.Count -ne 1) {
        throw "Connect exactly one Xiaomi marble in bootloader Fastboot mode (found $($Found.Count)), or pass -Serial."
    }
    $Serial = $Found[0]
}

function Get-FastbootVar([string]$Name) {
    $Result = Invoke-Fastboot @('-s',$Serial,'getvar',$Name)
    $Match = [regex]::Match($Result, '(?m)^(?:\(bootloader\)\s*)?' + [regex]::Escape($Name) + ':\s*(\S+)\s*$')
    if (!$Match.Success) {
        throw "Cannot read Fastboot variable: $Name"
    }
    return $Match.Groups[1].Value
}

function Convert-HexSize([string]$Value) {
    if ($Value -notmatch '^0x[0-9a-fA-F]+$') {
        throw "Invalid hex partition size: $Value"
    }
    return [Convert]::ToInt64($Value.Substring(2), 16)
}

function Get-ArtifactBytes([string]$RelPath) {
    $Item = @($Manifest.files | Where-Object { $_.path -eq $RelPath })
    if ($Item.Count -ne 1) { throw "Manifest missing $RelPath" }
    return [long]$Item[0].bytes
}

# Preflight device identity & partition checks (marble has NO init_boot)
$Product = Get-FastbootVar 'product'
if ($Product -ne 'marble') {
    throw "Unsupported device product '$Product'. Only Xiaomi marble (Redmi Note 12 Turbo / POCO F5) is supported."
}
if ((Get-FastbootVar 'unlocked') -ne 'yes') {
    throw 'Bootloader must already be unlocked.'
}
if ((Get-FastbootVar 'is-userspace') -ne 'no') {
    throw 'Device is in fastbootd. Reboot into bootloader Fastboot (Volume Down + Power) first.'
}
$CurrentSlot = Get-FastbootVar 'current-slot'
if ($CurrentSlot -notin @('a','b')) {
    throw "Unexpected current-slot value: $CurrentSlot"
}

foreach ($Part in @('boot','vendor_boot')) {
    $Cap = Convert-HexSize (Get-FastbootVar "partition-size:${Part}_a")
    $Need = Get-ArtifactBytes "images/$Part.img"
    if ($Cap -lt $Need) {
        throw "Partition ${Part}_a too small: capacity $Cap < image $Need"
    }
}

if ($Manifest.package_type -eq 'full') {
    $SuperCap = Convert-HexSize (Get-FastbootVar 'partition-size:super')
    if ($SuperCap -ne [long]$Manifest.super_bytes) {
        throw "Super partition size mismatch: device has $SuperCap bytes, expected $($Manifest.super_bytes) bytes."
    }
    foreach ($Part in @('vbmeta','vbmeta_system')) {
        $Cap = Convert-HexSize (Get-FastbootVar "partition-size:${Part}_a")
        $Need = Get-ArtifactBytes "images/$Part.img"
        if ($Cap -lt $Need) {
            throw "Partition ${Part}_a too small: capacity $Cap < image $Need"
        }
    }
}

Write-Host "Read-only preflight passed for Xiaomi marble ($Serial, current-slot=$CurrentSlot). Target slot: a." -ForegroundColor Green
if ($Mode -eq 'Inspect') {
    Write-Host "No partition was changed. Log: $LogPath"
    return
}

$ImgDir = Join-Path $PackageRoot 'images'

if ($Mode -eq 'Upgrade') {
    Write-Host ''
    Write-Host 'UPGRADE MODE (preserves super, userdata, and all stock firmware):' -ForegroundColor Cyan
    Write-Host '  - Will flash: boot_a, vendor_boot_a'
    Write-Host '  - Will run:   set_active a, reboot'
    Write-Host '  - Untouched:  super, userdata, vbmeta*, abl, xbl, modem*, dsp*, bluetooth*, persist, recovery*, dtbo*'
    Write-Host ''
    $Confirm = Read-Host 'Type UPGRADE MARBLE to continue'
    if ($Confirm -cne 'UPGRADE MARBLE') {
        throw 'Confirmation did not match. No partition was written.'
    }
    Write-Host 'Flashing boot_a...'
    Write-Host (Invoke-Fastboot @('-s',$Serial,'flash','boot_a',(Join-Path $ImgDir 'boot.img')))
    Write-Host 'Flashing vendor_boot_a...'
    Write-Host (Invoke-Fastboot @('-s',$Serial,'flash','vendor_boot_a',(Join-Path $ImgDir 'vendor_boot.img')))
    Write-Host 'Setting active slot to a...'
    Write-Host (Invoke-Fastboot @('-s',$Serial,'set_active','a'))
    Write-Host 'Upgrade succeeded; rebooting device...' -ForegroundColor Green
    Write-Host (Invoke-Fastboot @('-s',$Serial,'reboot'))
    Write-Host "Flash log: $LogPath"
    return
}

# FullInstall mode
Write-Host ''
Write-Host 'FULL INSTALL MODE (first-time AnyHMOS installation on Xiaomi marble):' -ForegroundColor Yellow
Write-Host '  - Will flash: vbmeta_a (--disable-verity --disable-verification)'
Write-Host '  - Will flash: vbmeta_system_a (--disable-verity --disable-verification)'
Write-Host '  - Will flash: boot_a, vendor_boot_a, super (9 GiB sparse; takes several minutes)'
Write-Host '  - Will run:   set_active a, reboot'
Write-Host '  - Untouched:  abl, xbl, modem*, dsp*, bluetooth*, persist, recovery*, dtbo*'
if ($FormatUserdataExt4) {
    Write-Host '  - WARNING:    -FormatUserdataExt4 ENABLED -> WILL ERASE metadata AND FORMAT userdata AS EXT4 (DELETES ALL USER DATA)!' -ForegroundColor Red
} else {
    Write-Host '  - Userdata:   NOT touched by this script. If migrating from stock encrypted Android/MIUI/HyperOS,' -ForegroundColor Yellow
    Write-Host '                you MUST format userdata as ext4 (in Recovery, or re-run with -FormatUserdataExt4) before first boot.' -ForegroundColor Yellow
}
Write-Host ''

$ConfirmFull = Read-Host 'Type FLASH MARBLE FULL to write system partitions'
if ($ConfirmFull -cne 'FLASH MARBLE FULL') {
    throw 'Confirmation did not match. No partition was written.'
}

if ($FormatUserdataExt4) {
    $ConfirmWipe = Read-Host 'Type FORMAT MARBLE USERDATA EXT4 to confirm DELETING ALL USER DATA'
    if ($ConfirmWipe -cne 'FORMAT MARBLE USERDATA EXT4') {
        throw 'Userdata format confirmation did not match. No partition was written.'
    }
}

Write-Host 'Flashing vbmeta_a (verity & verification disabled)...'
Write-Host (Invoke-Fastboot @('-s',$Serial,'flash','--disable-verity','--disable-verification','vbmeta_a',(Join-Path $ImgDir 'vbmeta.img')))
Write-Host 'Flashing vbmeta_system_a (verity & verification disabled)...'
Write-Host (Invoke-Fastboot @('-s',$Serial,'flash','--disable-verity','--disable-verification','vbmeta_system_a',(Join-Path $ImgDir 'vbmeta_system.img')))
Write-Host 'Flashing boot_a...'
Write-Host (Invoke-Fastboot @('-s',$Serial,'flash','boot_a',(Join-Path $ImgDir 'boot.img')))
Write-Host 'Flashing vendor_boot_a...'
Write-Host (Invoke-Fastboot @('-s',$Serial,'flash','vendor_boot_a',(Join-Path $ImgDir 'vendor_boot.img')))
Write-Host 'Flashing super (7+ GiB sparse image; please keep USB connected and wait)...'
Write-Host (Invoke-Fastboot @('-s',$Serial,'flash','super',(Join-Path $ImgDir 'super.img')))

if ($FormatUserdataExt4) {
    Write-Host 'Erasing Android encryption metadata...'
    Write-Host (Invoke-Fastboot @('-s',$Serial,'erase','metadata'))
    Write-Host 'Formatting userdata as ext4...'
    Write-Host (Invoke-Fastboot @('-s',$Serial,'format:ext4','userdata'))
}

Write-Host 'Setting active slot to a...'
Write-Host (Invoke-Fastboot @('-s',$Serial,'set_active','a'))
Write-Host 'All writes succeeded; rebooting device...' -ForegroundColor Green
Write-Host (Invoke-Fastboot @('-s',$Serial,'reboot'))
Write-Host "Flash log: $LogPath"
