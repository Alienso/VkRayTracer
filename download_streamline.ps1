# Puts the NVIDIA Streamline / DLSS Ray Reconstruction DLLs into lib/streamline (that folder is not in git).
# Source: the Streamline SDK releases, https://github.com/NVIDIA-RTX/Streamline/releases
#   lib/streamline/sl.interposer.dll   Streamline 2.14.1 (must match the headers in include/streamline)
#   lib/streamline/transformer/        Streamline's plugins + DLSS RR 310.9.1 (SDK 2.14.1): transformer models D-F
#   lib/streamline/cnn/                the same plugins + DLSS RR 310.3.0 (SDK 2.8.0), the last version with the CNN
#                                      models (A-C, plus transformer D-E); used when started with --dlss-cnn
# Only the production DLLs are extracted from the SDK zips (276 MB + 226 MB download, ~120 MB extracted).
# Files that are already there (in the right version) are skipped, so it is safe to run again.
#
#   powershell -ExecutionPolicy Bypass -File download_streamline.ps1
#   powershell -ExecutionPolicy Bypass -File download_streamline.ps1 -Sdk C:\Downloads\streamline-sdk-v2.14.1.zip `
#                                                                     -SdkCnn C:\Downloads\streamline-sdk-v2.8.0
#
# -Sdk / -SdkCnn: already downloaded SDKs 2.14.1 / 2.8.0, either the .zip or the extracted folder (no download).
param(
    [string]$Sdk = "",
    [string]$SdkCnn = "",
    [string]$Destination = (Join-Path $PSScriptRoot "lib\streamline")
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # the progress bar makes Invoke-WebRequest very slow in PowerShell 5
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# What goes where: file in the SDK's bin/x64 -> path under lib/streamline, optionally with the version it must have
$plugins = @("sl.common.dll", "sl.dlss_d.dll", "NvLowLatencyVk.dll")  # Streamline's plugins, Reflex for Vulkan
$sources = @(
    @{ Version = "2.14.1"; Local = $Sdk; Files = @(
        @{ Name = "sl.interposer.dll"; Target = "sl.interposer.dll" }
        $plugins | ForEach-Object { @{ Name = $_; Target = "transformer\$_" } }
        $plugins | ForEach-Object { @{ Name = $_; Target = "cnn\$_" } }
        @{ Name = "nvngx_dlssd.dll"; Target = "transformer\nvngx_dlssd.dll"; FileVersion = "310.9.1.0" }
        @{ Name = "nvngx_dlss.license.txt"; Target = "transformer\nvngx_dlss.license.txt" })
    },
    @{ Version = "2.8.0"; Local = $SdkCnn; Files = @(
        @{ Name = "nvngx_dlssd.dll"; Target = "cnn\nvngx_dlssd.dll"; FileVersion = "310.3.0.0" }
        @{ Name = "nvngx_dlss.license.txt"; Target = "cnn\nvngx_dlss.license.txt" })
    }
)

function Test-Present($file) {
    $path = Join-Path $Destination $file.Target
    if (-not (Test-Path $path)) {
        return $false
    }
    if ($file.FileVersion) {
        return (Get-Item $path).VersionInfo.FileVersion.Replace(",", ".") -eq $file.FileVersion
    }
    return $true
}

foreach ($folder in "", "transformer", "cnn") {
    New-Item -ItemType Directory -Force (Join-Path $Destination $folder) | Out-Null
}
foreach ($source in $sources) {
    $missing = @($source.Files | Where-Object { -not (Test-Present $_) })
    foreach ($file in $source.Files) {
        if ($missing -notcontains $file) {
            "exists     $($file.Target)"
        }
    }
    if ($missing.Count -eq 0) {
        continue
    }

    $version = $source.Version
    if ($source.Local -ne "" -and (Test-Path $source.Local -PathType Container)) {
        # Extracted SDK folder: plain copy
        foreach ($file in $missing) {
            Copy-Item -Force (Join-Path $source.Local "bin\x64\$($file.Name)") (Join-Path $Destination $file.Target)
            "copied     $($file.Target) (SDK $version)"
        }
        continue
    }

    $zipPath = $source.Local
    $downloaded = $false
    if ($zipPath -eq "") {
        $url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/streamline-sdk-v$version.zip"
        $zipPath = Join-Path ([IO.Path]::GetTempPath()) "streamline-sdk-v$version.zip"
        if (-not (Test-Path $zipPath)) {
            "downloading $url ..."
            Invoke-WebRequest -UseBasicParsing $url -OutFile "$zipPath.part"
            Move-Item -Force "$zipPath.part" $zipPath
        }
        $downloaded = $true
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        foreach ($file in $missing) {
            $entry = $zip.GetEntry("bin/x64/$($file.Name)")
            if ($entry -eq $null) {
                throw "bin/x64/$($file.Name) not found in $zipPath"
            }
            $path = Join-Path $Destination $file.Target
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$path.part", $true)
            Move-Item -Force "$path.part" $path
            "extracted  $($file.Target) ({0:N1} MB, SDK $version)" -f ($entry.Length / 1MB)
        }
    } finally {
        $zip.Dispose()
    }
    if ($downloaded) {
        Remove-Item $zipPath
    }
}

"done: the DLLs are in $Destination (copied next to the executable on the next build)"
