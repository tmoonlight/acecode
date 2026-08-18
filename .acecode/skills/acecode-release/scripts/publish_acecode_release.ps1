[CmdletBinding(DefaultParameterSetName = 'Release')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Release')]
    [Parameter(ParameterSetName = 'QuickValidation')]
    [ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$')]
    [string]$Version,

    [string]$Repo = (Get-Location).Path,
    [string]$UpdateDir = 'J:\jenkins_green\aupdate',
    [string]$RemoteBaseUrl = 'http://2017studio.imwork.net:82/aupdate/',
    [string]$Configuration = 'Release',
    [string]$Target = 'windows-x64',
    [string[]]$StageFiles = @(),
    [string]$CommitMessage = '',
    [string]$UpgradeTip = '',

    [Parameter(Mandatory = $true, ParameterSetName = 'QuickValidation')]
    [switch]$QuickValidation,

    [switch]$NoCommit,
    [switch]$NoTag,
    [switch]$Push,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$NoPublish,
    [switch]$AllowDirtyBuild
)

$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Read-Utf8Text {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Get-ProjectVersion {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $cmakePath = Join-Path $RepoRoot 'CMakeLists.txt'
    $cmake = Read-Utf8Text $cmakePath
    $match = [regex]::Match(
        $cmake,
        'project\(acecode VERSION (?<version>\d+\.\d+\.\d+) LANGUAGES C CXX\)')
    if (-not $match.Success) {
        throw 'Could not read the numeric ACECode project version from CMakeLists.txt.'
    }
    return $match.Groups['version'].Value
}

function Get-HighestStableVersion {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    $versions = New-Object 'System.Collections.Generic.List[System.Version]'
    $versions.Add([version](Get-ProjectVersion -RepoRoot $RepoRoot))
    if (Test-Path -LiteralPath $ManifestPath) {
        $manifest = (Read-Utf8Text $ManifestPath) | ConvertFrom-Json
        foreach ($release in @($manifest.releases)) {
            $releaseVersion = [string]$release.version
            if ($releaseVersion -match '^\d+\.\d+\.\d+$') {
                $versions.Add([version]$releaseVersion)
            }
        }
    }

    $highest = $versions[0]
    foreach ($candidate in $versions) {
        if ($candidate.CompareTo($highest) -gt 0) {
            $highest = $candidate
        }
    }
    return $highest
}

function Get-NextQuickValidationVersion {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    $highestStable = Get-HighestStableVersion -RepoRoot $RepoRoot -ManifestPath $ManifestPath
    $selectedCore = [version]("{0}.{1}.{2}" -f (
        $highestStable.Major,
        $highestStable.Minor,
        ($highestStable.Build + 1)))
    $highestPreNumber = 0

    if (Test-Path -LiteralPath $ManifestPath) {
        $manifest = (Read-Utf8Text $ManifestPath) | ConvertFrom-Json
        foreach ($release in @($manifest.releases)) {
            $releaseVersion = [string]$release.version
            if ($releaseVersion -notmatch '^(\d+)\.(\d+)\.(\d+)-pre\.(0|[1-9]\d*)$') {
                continue
            }

            $candidateCore = [version]("{0}.{1}.{2}" -f $Matches[1], $Matches[2], $Matches[3])
            $candidatePreNumber = [int]$Matches[4]
            $coreComparison = $candidateCore.CompareTo($selectedCore)
            if ($coreComparison -gt 0) {
                $selectedCore = $candidateCore
                $highestPreNumber = $candidatePreNumber
            } elseif ($coreComparison -eq 0 -and $candidatePreNumber -gt $highestPreNumber) {
                $highestPreNumber = $candidatePreNumber
            }
        }
    }

    return "{0}.{1}.{2}-pre.{3}" -f (
        $selectedCore.Major,
        $selectedCore.Minor,
        $selectedCore.Build,
        ($highestPreNumber + 1))
}

function Save-FileSnapshots {
    param([Parameter(Mandatory = $true)][string[]]$Paths)

    $snapshots = [ordered]@{}
    foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Version source file missing: $path"
        }
        $snapshots[$path] = [System.IO.File]::ReadAllBytes($path)
    }
    return ,$snapshots
}

function Restore-FileSnapshots {
    param([Parameter(Mandatory = $true)][System.Collections.IDictionary]$Snapshots)

    foreach ($path in $Snapshots.Keys) {
        [System.IO.File]::WriteAllBytes([string]$path, [byte[]]$Snapshots[$path])
    }
}

function Normalize-RepoPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $normalized = ($Path -replace '\\', '/').Trim()
    while ($normalized.StartsWith('./', [System.StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(2)
    }
    while ($normalized.StartsWith('/', [System.StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(1)
    }
    return $normalized
}

function Get-DirtyPaths {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $lines = & git -C $RepoRoot status --porcelain
    $paths = @()
    foreach ($line in $lines) {
        if ($line.Length -lt 4) { continue }
        $path = $line.Substring(3)
        if ($path -match ' -> ') {
            $path = ($path -split ' -> ')[-1]
        }
        $path = $path.Trim('"')
        if ($path) {
            $paths += (Normalize-RepoPath $path)
        }
    }
    return $paths
}

function Set-AcecodeVersion {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$NewVersion,
        [switch]$SkipVcpkgVersion
    )

    $numericVersion = ($NewVersion -split '-', 2)[0]
    $cmakePath = Join-Path $RepoRoot 'CMakeLists.txt'
    $cmake = [System.IO.File]::ReadAllText($cmakePath)
    $updatedCmake = [regex]::Replace(
        $cmake,
        'project\(acecode VERSION [0-9A-Za-z.\-]+ LANGUAGES C CXX\)',
        "project(acecode VERSION $numericVersion LANGUAGES C CXX)",
        1)
    if ($updatedCmake -eq $cmake -and $cmake -notmatch "project\(acecode VERSION $([regex]::Escape($numericVersion)) LANGUAGES C CXX\)") {
        throw 'Could not update CMakeLists.txt project version.'
    }
    Write-Utf8NoBom $cmakePath $updatedCmake

    $vcpkgPath = Join-Path $RepoRoot 'vcpkg.json'
    if (-not $SkipVcpkgVersion -and (Test-Path -LiteralPath $vcpkgPath)) {
        $vcpkg = Get-Content -LiteralPath $vcpkgPath -Raw | ConvertFrom-Json
        $vcpkg.'version-semver' = $NewVersion
        $json = $vcpkg | ConvertTo-Json -Depth 16
        Write-Utf8NoBom $vcpkgPath ($json + [Environment]::NewLine)
    }
}

function Set-QuickValidationVersionTemplate {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$NewVersion
    )

    $templatePath = Join-Path $RepoRoot 'src\version.hpp.in'
    $template = Read-Utf8Text $templatePath
    $placeholder = '#define ACECODE_VERSION "@acecode_VERSION@"'
    if (-not $template.Contains($placeholder)) {
        throw 'Could not find the ACECODE_VERSION placeholder in src/version.hpp.in.'
    }
    $updated = $template.Replace(
        $placeholder,
        "#define ACECODE_VERSION `"$NewVersion`"")
    Write-Utf8NoBom $templatePath $updated
}

function Ensure-ZipMimeConfig {
    param([Parameter(Mandatory = $true)][string]$Directory)
    $webConfig = Join-Path $Directory 'web.config'
    if (Test-Path -LiteralPath $webConfig) {
        $text = [System.IO.File]::ReadAllText($webConfig)
        if ($text -match 'mimeType="application/zip"') {
            return
        }
    }

    $xml = @'
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
    <system.webServer>
        <staticContent>
            <remove fileExtension=".zip" />
            <mimeMap fileExtension=".zip" mimeType="application/zip" />
        </staticContent>
    </system.webServer>
</configuration>
'@
    Write-Utf8NoBom $webConfig ($xml + [Environment]::NewLine)
}

function Update-Manifest {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$NewVersion,
        [Parameter(Mandatory = $true)][string]$TargetName,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string]$Sha256,
        [Parameter(Mandatory = $true)][UInt64]$Size,
        [Parameter(Mandatory = $true)][string]$UpgradeTip,
        [object[]]$ExtraPackages = @()
    )

    $oldReleases = @()
    if (Test-Path -LiteralPath $ManifestPath) {
        $oldManifest = (Read-Utf8Text $ManifestPath) | ConvertFrom-Json
        if ($oldManifest.releases) {
            foreach ($release in $oldManifest.releases) {
                if ($release.version -ne $NewVersion) {
                    $oldReleases += $release
                }
            }
        }
    }

    $packages = @(
        [ordered]@{
            target = $TargetName
            file = $FileName
            sha256 = $Sha256
            size = $Size
        }
    )
    foreach ($package in $ExtraPackages) {
        $packages += $package
    }

    $newRelease = [ordered]@{
        version = $NewVersion
        published_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        notes = $UpgradeTip
        packages = $packages
    }

    $manifest = [ordered]@{
        schema_version = 1
        latest = $NewVersion
        releases = @($newRelease) + $oldReleases
    }
    Write-Utf8NoBom $ManifestPath (($manifest | ConvertTo-Json -Depth 16) + [Environment]::NewLine)
}

function Verify-HttpPackage {
    param(
        [Parameter(Mandatory = $true)][string]$BaseUrl,
        [Parameter(Mandatory = $true)][string]$VersionToVerify,
        [Parameter(Mandatory = $true)][string]$PackageName,
        [Parameter(Mandatory = $true)][UInt64]$ExpectedSize,
        [Parameter(Mandatory = $true)][string]$ExpectedSha,
        [Parameter(Mandatory = $true)][string]$ExpectedUpgradeTip
    )

    $base = $BaseUrl.TrimEnd('/') + '/'
    $manifestTmp = Join-Path $env:TEMP ("acecode-release-manifest-" + [guid]::NewGuid().ToString('N') + '.json')
    $tmp = Join-Path $env:TEMP ("acecode-release-verify-" + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        Invoke-WebRequest -Uri ($base + 'aceupdate.json') -UseBasicParsing -TimeoutSec 15 -OutFile $manifestTmp
        $manifest = (Read-Utf8Text $manifestTmp) | ConvertFrom-Json
        if ($manifest.latest -ne $VersionToVerify) {
            throw "HTTP manifest latest is '$($manifest.latest)', expected '$VersionToVerify'."
        }
        $release = @($manifest.releases) |
            Where-Object { $_.version -eq $VersionToVerify } |
            Select-Object -First 1
        if ($null -eq $release) {
            throw "HTTP manifest release '$VersionToVerify' is missing."
        }
        if (([string]$release.notes) -cne $ExpectedUpgradeTip) {
            throw "HTTP manifest upgrade tip does not match the published -UpgradeTip."
        }

        Invoke-WebRequest -Uri ($base + $PackageName) -UseBasicParsing -TimeoutSec 60 -Headers @{ Accept = 'application/zip' } -OutFile $tmp
        $actualSize = [UInt64](Get-Item -LiteralPath $tmp).Length
        $actualSha = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualSize -ne $ExpectedSize) {
            throw "HTTP package size mismatch: got $actualSize, expected $ExpectedSize."
        }
        if ($actualSha -ne $ExpectedSha) {
            throw "HTTP package sha256 mismatch: got $actualSha, expected $ExpectedSha."
        }
    } finally {
        Remove-Item -LiteralPath $manifestTmp -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

function New-ZipFromDirectoryWithForwardSlashes {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $sourceRoot = (Resolve-Path -LiteralPath $SourceDirectory).Path
    $destDir = Split-Path -Parent $DestinationPath
    if ($destDir) {
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    }
    if (Test-Path -LiteralPath $DestinationPath) {
        Remove-Item -LiteralPath $DestinationPath -Force
    }

    $archive = [System.IO.Compression.ZipFile]::Open(
        $DestinationPath,
        [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $files = Get-ChildItem -LiteralPath $sourceRoot -File -Recurse
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\', '/')
            $entryName = $relative -replace '\\', '/'
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive,
                $file.FullName,
                $entryName,
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
}

function Remove-PackageStage {
    param(
        [Parameter(Mandatory = $true)][string]$StagePath,
        [Parameter(Mandatory = $true)][string]$PackageRoot
    )

    New-Item -ItemType Directory -Force -Path $PackageRoot | Out-Null
    $packageRootResolved = (Resolve-Path -LiteralPath $PackageRoot).Path
    if (Test-Path -LiteralPath $StagePath) {
        $stageResolved = (Resolve-Path -LiteralPath $StagePath).Path
        $rootWithSeparator = $packageRootResolved.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
        if (-not $stageResolved.StartsWith($rootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove staging path outside package root: $stageResolved"
        }
        Remove-Item -LiteralPath $stageResolved -Recurse -Force
    }
}

function Test-ZipEntries {
    param(
        [Parameter(Mandatory = $true)][string]$ZipPath,
        [string[]]$RequiredEntries = @(),
        [string[]]$ForbiddenPrefixes = @()
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $entries = $archive.Entries | ForEach-Object { $_.FullName }
        foreach ($entry in $entries) {
            if ($entry -match '\\') {
                throw "Package entry must use forward slashes: $entry"
            }
            foreach ($prefix in $ForbiddenPrefixes) {
                if ($entry.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                    throw "Package must not contain entry under ${prefix}: $entry"
                }
            }
        }
        foreach ($required in $RequiredEntries) {
            if ($entries -notcontains $required) {
                throw "Package missing required entry: $required"
            }
        }
    } finally {
        $archive.Dispose()
    }
}

$Repo = (Resolve-Path -LiteralPath $Repo).Path
if (-not (Test-Path -LiteralPath (Join-Path $Repo 'CMakeLists.txt'))) {
    throw "Not an ACECode repo root: $Repo"
}

$manifestPath = Join-Path $UpdateDir 'aceupdate.json'
if ($QuickValidation) {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = Get-NextQuickValidationVersion -RepoRoot $Repo -ManifestPath $manifestPath
    }
    if ($Version -notmatch '^\d+\.\d+\.\d+-pre\.(0|[1-9]\d*)$') {
        throw 'Quick validation requires a prerelease version such as 0.8.7-pre.1.'
    }
    if ($Push) {
        throw 'Quick validation never pushes Git commits or tags. Remove -Push.'
    }
    if ($NoPublish) {
        throw 'Quick validation must publish its Windows package. Remove -NoPublish.'
    }
    if ($Target -ne 'windows-x64') {
        throw 'Quick validation only supports the windows-x64 target.'
    }
    if (@($StageFiles).Count -gt 0 -or -not [string]::IsNullOrWhiteSpace($CommitMessage)) {
        throw 'Quick validation does not commit files. Remove -StageFiles and -CommitMessage.'
    }

    $highestStable = Get-HighestStableVersion -RepoRoot $Repo -ManifestPath $manifestPath
    $quickVersionCore = [version](($Version -split '-', 2)[0])
    if ($quickVersionCore.CompareTo($highestStable) -le 0) {
        throw "Quick validation version '$Version' must have a numeric core newer than stable version '$highestStable'."
    }
    if ([string]::IsNullOrWhiteSpace($UpgradeTip)) {
        $UpgradeTip = "ACECode $Version Windows prerelease validation package."
    } else {
        $UpgradeTip = $UpgradeTip.Trim()
    }
} else {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') {
        throw 'Stable releases require a version such as 0.8.7. Use -QuickValidation for x.y.z-pre.N packages.'
    }
    if (-not $NoPublish) {
        if ([string]::IsNullOrWhiteSpace($UpgradeTip)) {
            throw 'Publishing to aupdate requires a non-empty -UpgradeTip.'
        }
        $UpgradeTip = $UpgradeTip.Trim()
    }
}

$tag = "v$Version"
if (-not $QuickValidation -and -not $CommitMessage) {
    $CommitMessage = "Release $tag"
}

if ($QuickValidation) {
    Write-Host "== ACECode quick validation $Version =="
} else {
    Write-Host "== ACECode release $Version =="
}
Write-Host "Repo:      $Repo"
Write-Host "UpdateDir: $UpdateDir"

$dirtyBeforeVersionOverride = @()
$versionSnapshots = $null
if ($QuickValidation) {
    $dirtyBeforeVersionOverride = @(Get-DirtyPaths -RepoRoot $Repo)
    $versionSourcePaths = @(
        (Join-Path $Repo 'CMakeLists.txt'),
        (Join-Path $Repo 'src\version.hpp.in')
    )
    $versionSnapshots = Save-FileSnapshots -Paths $versionSourcePaths
}

$head = $null
try {
    if ($QuickValidation) {
        Set-AcecodeVersion -RepoRoot $Repo -NewVersion $Version -SkipVcpkgVersion
        Set-QuickValidationVersionTemplate -RepoRoot $Repo -NewVersion $Version
    } else {
        Set-AcecodeVersion -RepoRoot $Repo -NewVersion $Version
    }

    $defaultStage = @('CMakeLists.txt', 'vcpkg.json')
    $stageSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($path in ($defaultStage + $StageFiles)) {
        if (-not [string]::IsNullOrWhiteSpace($path)) {
            [void]$stageSet.Add((Normalize-RepoPath $path))
        }
    }

    if ($QuickValidation) {
        if ($dirtyBeforeVersionOverride.Count -gt 0) {
            Write-Host 'Quick validation includes current working tree changes:'
            $dirtyBeforeVersionOverride | ForEach-Object { Write-Host "  $_" }
        }
    } else {
        $dirty = Get-DirtyPaths -RepoRoot $Repo
        $outside = @()
        foreach ($path in $dirty) {
            if (-not $stageSet.Contains($path)) {
                $outside += $path
            }
        }
        if ($outside.Count -gt 0 -and -not $AllowDirtyBuild) {
            Write-Host "Dirty files outside release file set:"
            $outside | ForEach-Object { Write-Host "  $_" }
            throw 'Refusing to build/package a release from unrelated dirty files. Add intended files with -StageFiles, clean the tree, or pass -AllowDirtyBuild.'
        }
    }

    if (-not $SkipBuild) {
        if ($QuickValidation) {
            Invoke-Native cmake --build (Join-Path $Repo 'build') --config $Configuration --target acecode acecode-desktop
        } else {
            Invoke-Native cmake --build (Join-Path $Repo 'build') --config $Configuration --target acecode acecode-desktop acecode_unit_tests
        }
    }

    if (-not $QuickValidation -and -not $SkipTests) {
        $testExe = Join-Path $Repo "build\tests\$Configuration\acecode_unit_tests.exe"
        Invoke-Native $testExe '--gtest_filter=Upgrade*:*ConfigUpgrade*'
    }

    $exe = Join-Path $Repo "build\$Configuration\acecode.exe"
    $desktopExe = Join-Path $Repo "build\$Configuration\acecode-desktop.exe"
    $versionOutput = (& $exe --version).Trim()
    if ($versionOutput -ne "acecode v$Version") {
        throw "Built executable reports '$versionOutput', expected 'acecode v$Version'."
    }
    if (-not (Test-Path -LiteralPath $desktopExe)) {
        throw "Desktop executable missing: $desktopExe. Configure the build with -DACECODE_BUILD_DESKTOP=ON before release packaging."
    }

    if (-not $QuickValidation -and -not $NoCommit) {
        $filesToAdd = @()
        foreach ($path in $stageSet) {
            $candidate = Join-Path $Repo ($path -replace '/', '\')
            if (Test-Path -LiteralPath $candidate) {
                $filesToAdd += $path
            }
        }
        if ($filesToAdd.Count -gt 0) {
            Invoke-Native -FilePath git -Arguments (@('-C', $Repo, 'add', '--') + $filesToAdd)
        }
        & git -C $Repo diff --cached --quiet
        if ($LASTEXITCODE -eq 1) {
            Invoke-Native -FilePath git -Arguments @('-C', $Repo, 'commit', '-m', $CommitMessage)
        } elseif ($LASTEXITCODE -ne 0) {
            throw 'git diff --cached failed.'
        } else {
            Write-Host 'No staged changes to commit; using current HEAD.'
        }
    }

    if (-not $QuickValidation -and -not $NoTag) {
        & git -C $Repo rev-parse -q --verify "refs/tags/$tag" *> $null
        if ($LASTEXITCODE -eq 0) {
            throw "Tag already exists: $tag"
        }
        Invoke-Native -FilePath git -Arguments @('-C', $Repo, 'tag', '-a', $tag, '-m', "ACECode $tag")
    }

    if ($Push) {
        Invoke-Native -FilePath git -Arguments @('-C', $Repo, 'push', 'origin', 'HEAD')
        if (-not $NoTag) {
            Invoke-Native -FilePath git -Arguments @('-C', $Repo, 'push', 'origin', $tag)
        }
    }

    if (-not $NoPublish) {
        $pkgName = "acecode-$Version-$Target"
        $packageRoot = Join-Path $Repo 'build\package'
        $stage = Join-Path $packageRoot $pkgName
        $zipPath = Join-Path $UpdateDir "$pkgName.zip"

        Remove-PackageStage -StagePath $stage -PackageRoot $packageRoot

        New-Item -ItemType Directory -Force -Path (Join-Path $stage 'share\acecode') | Out-Null
        Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'acecode.exe') -Force
        Copy-Item -LiteralPath $desktopExe -Destination (Join-Path $stage 'acecode-desktop.exe') -Force
        Copy-Item -LiteralPath (Join-Path $Repo 'assets\models_dev') -Destination (Join-Path $stage 'share\acecode\models_dev') -Recurse -Force
        Copy-Item -LiteralPath (Join-Path $Repo 'assets\seed') -Destination (Join-Path $stage 'share\acecode\seed') -Recurse -Force

        New-Item -ItemType Directory -Force -Path $UpdateDir | Out-Null
        Ensure-ZipMimeConfig -Directory $UpdateDir
        New-ZipFromDirectoryWithForwardSlashes -SourceDirectory $stage -DestinationPath $zipPath

        Test-ZipEntries -ZipPath $zipPath -RequiredEntries @(
            'acecode.exe',
            'acecode-desktop.exe',
            'share/acecode/models_dev/api.json',
            'share/acecode/seed/MANIFEST.json'
        ) -ForbiddenPrefixes @('ace-browser-')

        $size = [UInt64](Get-Item -LiteralPath $zipPath).Length
        $sha = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()

        Update-Manifest -ManifestPath $manifestPath -NewVersion $Version -TargetName $Target -FileName "$pkgName.zip" -Sha256 $sha -Size $size -UpgradeTip $UpgradeTip

        if ($RemoteBaseUrl) {
            Verify-HttpPackage -BaseUrl $RemoteBaseUrl -VersionToVerify $Version -PackageName "$pkgName.zip" -ExpectedSize $size -ExpectedSha $sha -ExpectedUpgradeTip $UpgradeTip
        }

        Write-Host "Package: $zipPath"
        Write-Host "Size:    $size"
        Write-Host "SHA256:  $sha"
    }

    $head = (& git -C $Repo rev-parse --short HEAD).Trim()
} finally {
    if ($QuickValidation -and $null -ne $versionSnapshots) {
        Restore-FileSnapshots -Snapshots $versionSnapshots
    }
}

if ($QuickValidation) {
    Write-Host "Quick validation package complete: $Version from $head (no commit or tag created)"
} else {
    Write-Host "Release complete: $tag at $head"
}
