# Stage the current Windows Release binaries and compile the ACECode installer.
[CmdletBinding()]
param(
    [string]$RepoRoot = "",
    [string]$Version = "",
    [string]$Iscc = "",
    [string]$DesktopExe = "",
    [string]$CliExe = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

$installerDir = Join-Path $RepoRoot "installer\windows"
$stagingDir = Join-Path $installerDir "staging"
$imagesDir = Join-Path $installerDir "images"
$issPath = Join-Path $installerDir "acecode.iss"

function Find-ExistingFile([string[]]$candidates) {
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

if (-not $Version) {
    $cmake = Get-Content -LiteralPath (Join-Path $RepoRoot "CMakeLists.txt") -Raw
    if ($cmake -match 'project\(acecode VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    } else {
        throw "Unable to read ACECode version from CMakeLists.txt"
    }
}

if (-not $Iscc) {
    $Iscc = Find-ExistingFile @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    )
}
if (-not $Iscc) {
    throw "ISCC.exe was not found. Install Inno Setup 6 or 7."
}

$cli = Find-ExistingFile @(
    $CliExe,
    (Join-Path $RepoRoot "build\Release\acecode.exe"),
    (Join-Path $RepoRoot "build\MinSizeRel\acecode.exe"),
    (Join-Path $RepoRoot "build\acecode.exe")
)
$desktop = Find-ExistingFile @(
    $DesktopExe,
    (Join-Path $RepoRoot "build\Release\acecode-desktop.exe"),
    (Join-Path $RepoRoot "build\MinSizeRel\acecode-desktop.exe"),
    (Join-Path $RepoRoot "build\acecode-desktop.exe")
)
if (-not $cli) { throw "Missing acecode.exe. Build the Release desktop/CLI binaries first." }
if (-not $desktop) { throw "Missing acecode-desktop.exe. Build the Release desktop binary first." }

Write-Host "Staging installer payload from:"
Write-Host "  CLI     $cli"
Write-Host "  Desktop $desktop"

if (Test-Path -LiteralPath $stagingDir) {
    Remove-Item -LiteralPath $stagingDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDir | Out-Null
Copy-Item -LiteralPath $cli -Destination (Join-Path $stagingDir "acecode.exe")
Copy-Item -LiteralPath $desktop -Destination (Join-Path $stagingDir "acecode-desktop.exe")
foreach ($name in @("README.md", "README_CN.md")) {
    $source = Join-Path $RepoRoot $name
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $stagingDir $name)
    }
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw "cmake is required to stage share/acecode assets"
}
$buildDir = Join-Path $RepoRoot "build"
& cmake --install $buildDir --config Release --prefix $stagingDir --component models_dev_registry
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install the bundled models.dev registry"
}
$seedSource = Join-Path $RepoRoot "assets\seed"
$seedDest = Join-Path $stagingDir "share\acecode\seed"
if (-not (Test-Path -LiteralPath $seedSource)) {
    throw "Missing assets\seed"
}
New-Item -ItemType Directory -Path (Split-Path $seedDest) -Force | Out-Null
Copy-Item -LiteralPath $seedSource -Destination $seedDest -Recurse -Force

python (Join-Path $installerDir "generate_wizard_art.py")
if ($LASTEXITCODE -ne 0) {
    throw "Failed to generate Inno wizard artwork"
}

& $Iscc "/DMyAppVersion=$Version" $issPath
if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed"
}

$setup = Join-Path $installerDir "output\ACECode-$Version-windows-x64-setup.exe"
if (-not (Test-Path -LiteralPath $setup)) {
    throw "Installer was not produced: $setup"
}
Write-Host "Installer ready: $setup"
Write-Host ("Size: {0:N1} MB" -f ((Get-Item $setup).Length / 1MB))
