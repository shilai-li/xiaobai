param(
    [string]$EspProjectDirectory = "",
    [string]$EspIdfProfile = "C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = $PSScriptRoot
if (-not $EspProjectDirectory) { $EspProjectDirectory = Join-Path $repoRoot 'xiaozhi-esp32' }
if (-not (Test-Path -LiteralPath $EspProjectDirectory)) { throw "ESP32 project not found: $EspProjectDirectory" }
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string[]]$Lines
    )
    [System.IO.File]::WriteAllLines($Path, $Lines, $utf8NoBom)
}

if ($SkipBuild) {
    # Existing build packaging only requires Python with esptool installed.
} else {
    if (-not (Test-Path -LiteralPath $EspIdfProfile)) { throw "ESP-IDF profile not found: $EspIdfProfile" }
    . $EspIdfProfile
}

Push-Location $EspProjectDirectory
try {
    if (-not $SkipBuild) {
        idf.py build
        if ($LASTEXITCODE -ne 0) { throw "ESP32 build failed: $LASTEXITCODE" }
    }

    $buildDirectory = Join-Path $EspProjectDirectory 'build'
    $flashArgsPath = Join-Path $buildDirectory 'flash_args'
    if (-not (Test-Path -LiteralPath $flashArgsPath)) { throw "Flash arguments not found: $flashArgsPath" }

    $packageDirectory = Join-Path $buildDirectory 'esp32-flash-package'
    $legacyPackageZip = Join-Path $buildDirectory 'esp32-flash-package.zip'
    if (Test-Path -LiteralPath $packageDirectory) { Remove-Item -LiteralPath $packageDirectory -Recurse -Force }
    if (Test-Path -LiteralPath $legacyPackageZip -PathType Leaf) { Remove-Item -LiteralPath $legacyPackageZip -Force }
    New-Item -ItemType Directory -Path $packageDirectory | Out-Null

    $flashArgs = Get-Content -LiteralPath $flashArgsPath
    $coreFlashArgs = @()
    foreach ($line in $flashArgs) {
        $match = [regex]::Match($line.Trim(), '^(0x[0-9A-Fa-f]+)\s+(.+)$')
        if (-not $match.Success) { continue }

        $relativeFile = $match.Groups[2].Value.Trim()
        if ([System.IO.Path]::GetFileName($relativeFile) -match '(?i)^nvs.*\.bin$') { continue }
        if ($relativeFile.Contains('..')) { throw "Unsupported path in flash_args: $relativeFile" }
        $sourceFile = Join-Path $buildDirectory ($relativeFile -replace '/', '\')
        if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) { throw "Flash image not found: $sourceFile" }

        $destinationFile = Join-Path $packageDirectory ($relativeFile -replace '/', '\')
        $destinationDirectory = Split-Path $destinationFile -Parent
        New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
        Copy-Item -LiteralPath $sourceFile -Destination $destinationFile -Force
        $coreFlashArgs += $line
    }

    $coreArgsName = 'core-firmware.args'
    $coreImageName = 'esp32-core-firmware.bin'
    Write-Utf8NoBom -Path (Join-Path $packageDirectory $coreArgsName) -Lines $coreFlashArgs
    Push-Location $packageDirectory
    try {
        python -m esptool --chip esp32c3 merge_bin -o $coreImageName "@$coreArgsName"
        if ($LASTEXITCODE -ne 0) { throw "Core firmware merge failed: $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
    Remove-Item -LiteralPath (Join-Path $packageDirectory $coreArgsName) -Force
    Get-ChildItem -LiteralPath $packageDirectory -File |
        Where-Object { $_.Name -ne $coreImageName } |
        Remove-Item -Force
    Get-ChildItem -LiteralPath $packageDirectory -Directory |
        Remove-Item -Recurse -Force

    $readmeLines = @(
        '# ESP32-C3 Merged Core Firmware Package'
        ''
        'This package contains one merged core firmware image. It does not generate or include an NVS image.'
        ''
        '## Initial firmware flashing'
        ''
        'The merged image covers the firmware address range from `0x0` and the NVS gap. Use it after a full erase on a blank device:'
        ''
        '```powershell'
        'python -m esptool --chip esp32c3 --port COMx --baud 921600 erase_flash'
        'python -m esptool --chip esp32c3 --port COMx --baud 921600 write_flash 0x0 esp32-core-firmware.bin'
        '```'
        ''
        'After flashing, use `tools/production_sn/production_sn.py` to write and verify `wifi/serial_number`.'
        'Do not use this merged image for updates when existing NVS data must be preserved.'
    )
    Write-Utf8NoBom -Path (Join-Path $packageDirectory 'README.md') -Lines $readmeLines

    if (Test-Path -LiteralPath (Join-Path $buildDirectory 'merged-firmware.bin')) {
        Write-Warning 'A legacy merged-firmware.bin still exists in build. It is not included in this package; use esp32-core-firmware.bin for the clean merged image.'
    }
    Write-Host "ESP32 merged core firmware package: $packageDirectory" -ForegroundColor Green
} finally {
    Pop-Location
    if (Get-Command deactivate -ErrorAction SilentlyContinue) { deactivate }
}
