# Puts the NVIDIA Streamline / DLSS Ray Reconstruction DLLs into lib/streamline (that folder is not in git).
# Source: the Streamline SDK release, https://github.com/NVIDIA-RTX/Streamline/releases
# Only the production DLLs we load are extracted from the SDK zip (276 MB download, ~50 MB extracted).
# Files that already exist are skipped, so it is safe to run again.
#
#   powershell -ExecutionPolicy Bypass -File download_streamline.ps1
#   powershell -ExecutionPolicy Bypass -File download_streamline.ps1 -Sdk C:\Downloads\streamline-sdk-v2.14.1.zip
#
# -Sdk: an already downloaded SDK, either the .zip or the extracted folder (skips the download).
param(
    [string]$Sdk = "",
    [string]$Destination = (Join-Path $PSScriptRoot "lib\streamline")
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # the progress bar makes Invoke-WebRequest very slow in PowerShell 5
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Must match the headers in include/streamline
$version = "2.14.1"
$url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/streamline-sdk-v$version.zip"

# In the SDK: bin/x64 holds the production builds (bin/x64/development has the debug versions)
$files = @(
    "sl.interposer.dll",       # Streamline entry point, loaded by source/Dlss.cpp
    "sl.common.dll",           # Streamline plugins
    "sl.dlss_d.dll",
    "nvngx_dlssd.dll",         # DLSS Ray Reconstruction itself (48 MB)
    "NvLowLatencyVk.dll",      # Reflex for Vulkan, loaded by sl.common
    "nvngx_dlss.license.txt"
)

New-Item -ItemType Directory -Force $Destination | Out-Null
$missing = @($files | Where-Object { -not (Test-Path (Join-Path $Destination $_)) })
foreach ($name in $files) {
    if ($missing -notcontains $name) {
        "exists     $name"
    }
}
if ($missing.Count -eq 0) {
    "done: nothing to do, the DLLs are in $Destination"
    return
}

if ($Sdk -ne "" -and (Test-Path $Sdk -PathType Container)) {
    # Extracted SDK folder: plain copy
    foreach ($name in $missing) {
        Copy-Item (Join-Path $Sdk "bin\x64\$name") (Join-Path $Destination $name)
        "copied     $name"
    }
} else {
    $zipPath = $Sdk
    $downloaded = $false
    if ($zipPath -eq "") {
        $zipPath = Join-Path ([IO.Path]::GetTempPath()) "streamline-sdk-v$version.zip"
        if (-not (Test-Path $zipPath)) {
            "downloading $url (276 MB) ..."
            Invoke-WebRequest -UseBasicParsing $url -OutFile "$zipPath.part"
            Move-Item -Force "$zipPath.part" $zipPath
        }
        $downloaded = $true
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        foreach ($name in $missing) {
            $entry = $zip.GetEntry("bin/x64/$name")
            if ($entry -eq $null) {
                throw "bin/x64/$name not found in $zipPath"
            }
            $path = Join-Path $Destination $name
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$path.part", $true)
            Move-Item -Force "$path.part" $path
            "extracted  $name ({0:N1} MB)" -f ($entry.Length / 1MB)
        }
    } finally {
        $zip.Dispose()
    }
    if ($downloaded) {
        Remove-Item $zipPath
    }
}

"done: the DLLs are in $Destination (copied next to the executable on the next build)"
