# Downloads the glTF test models into resource/models (that folder is not in git).
# Source: the Khronos glTF sample assets, https://github.com/KhronosGroup/glTF-Sample-Assets
# Files that already exist are skipped, so it is safe to run again.
#
#   powershell -ExecutionPolicy Bypass -File download_models.ps1
param([string]$Destination = (Join-Path $PSScriptRoot "resource\models"))

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # the progress bar makes Invoke-WebRequest very slow in PowerShell 5
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repo = "KhronosGroup/glTF-Sample-Assets"
$raw = "https://raw.githubusercontent.com/$repo/main/Models"

# Single-file models (.glb: everything packed into one file)
$glbModels = @(
    "DamagedHelmet", "Duck", "Lantern",                   # Phase 3: loading, textures, normal maps
    "MetalRoughSpheres", "TransmissionTest",              # Phase 5/6: metal/roughness, glass
    "AlphaBlendModeTest", "DragonAttenuation",            # Phase 6: alpha, solid glass with absorption
    "TextureTransformMultiTest"                           # KHR_texture_transform on every texture slot
)
# Multi-file models (.gltf + .bin + images), each in a folder of its own
$folderModels = @("Sponza", "TextureTransformTest")  # TextureTransformTest: KHR_texture_transform, base color

New-Item -ItemType Directory -Force $Destination | Out-Null

function Get-File([string]$url, [string]$path) {
    if (Test-Path $path) {
        return $false
    }
    $temporary = "$path.part"  # an interrupted download doesn't leave a broken file that would be skipped next time
    Invoke-WebRequest -UseBasicParsing $url -OutFile $temporary
    Move-Item -Force $temporary $path
    return $true
}

foreach ($name in $glbModels) {
    $path = Join-Path $Destination "$name.glb"
    if (Get-File "$raw/$name/glTF-Binary/$name.glb" $path) {
        "downloaded $name.glb ({0:N1} MB)" -f ((Get-Item $path).Length / 1MB)
    } else {
        "exists     $name.glb"
    }
}

foreach ($name in $folderModels) {
    # Ask GitHub which files the folder holds (.gltf, .bin, textures)
    $listing = Invoke-WebRequest -UseBasicParsing "https://api.github.com/repos/$repo/contents/Models/$name/glTF"
    $files = $listing.Content | ConvertFrom-Json
    $folder = Join-Path $Destination $name
    New-Item -ItemType Directory -Force $folder | Out-Null
    $new = 0
    foreach ($file in $files) {
        if (Get-File $file.download_url (Join-Path $folder $file.name)) {
            $new++
        }
    }
    $size = (Get-ChildItem $folder -File | Measure-Object Length -Sum).Sum / 1MB
    "{0,-10} $name/ ({1} files, {2} new, {3:N1} MB)" -f ($(if ($new -gt 0) { "downloaded" } else { "exists" })), $files.Count, $new, $size
}

"done: models are in $Destination (copied next to the executable on the next build)"
