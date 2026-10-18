[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$UpgradeTip,

    [string]$RepoSlug = 'tmoonlight/acecode',
    [string]$UpdateDir = 'J:\jenkins_green\aupdate',
    [string]$RemoteBaseUrl = 'http://2017studio.imwork.net:82/aupdate/',
    [ValidateRange(1, 180)]
    [int]$WaitMinutes = 45,

    # Test/repair input. Production releases should omit this so assets are
    # fetched from the published GitHub Release.
    [string]$AssetDirectory = '',
    [string]$PublishedAt = ''
)

$ErrorActionPreference = 'Stop'

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

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

function Get-AssetDefinitions {
    param([Parameter(Mandatory = $true)][string]$ReleaseVersion)

    return @(
        [pscustomobject]@{
            Target = 'windows-x64'
            Source = 'acecode-windows-x64.zip'
            Versioned = "acecode-$ReleaseVersion-windows-x64.zip"
            Alias = 'acecode-windows-x64.zip'
            Kind = 'windows'
            ContentType = 'application/zip'
            Arch = 'x64'
        },
        [pscustomobject]@{
            Target = 'windows-arm64'
            Source = 'acecode-windows-arm64.zip'
            Versioned = "acecode-$ReleaseVersion-windows-arm64.zip"
            Alias = 'acecode-windows-arm64.zip'
            Kind = 'windows'
            ContentType = 'application/zip'
            Arch = 'arm64'
        },
        [pscustomobject]@{
            Target = 'macos-x64'
            Source = "ACECode-$ReleaseVersion-macos-x64-update.zip"
            Versioned = "ACECode-$ReleaseVersion-macos-x64-update.zip"
            Alias = 'ACECode-macos-x64-update.zip'
            Kind = 'macos'
            ContentType = 'application/zip'
            Arch = 'x64'
        },
        [pscustomobject]@{
            Target = 'macos-arm64'
            Source = "ACECode-$ReleaseVersion-macos-arm64-update.zip"
            Versioned = "ACECode-$ReleaseVersion-macos-arm64-update.zip"
            Alias = 'ACECode-macos-arm64-update.zip'
            Kind = 'macos'
            ContentType = 'application/zip'
            Arch = 'arm64'
        },
        [pscustomobject]@{
            Target = 'linux-x64-updater-v1'
            Source = "acecode-$ReleaseVersion-linux-x64-update.zip"
            Versioned = "acecode-$ReleaseVersion-linux-x64-update.zip"
            Alias = 'acecode-linux-x64-update.zip'
            Kind = 'linux'
            ContentType = 'application/zip'
            Arch = 'x64'
        },
        [pscustomobject]@{
            Target = 'linux-arm64-updater-v1'
            Source = "acecode-$ReleaseVersion-linux-arm64-update.zip"
            Versioned = "acecode-$ReleaseVersion-linux-arm64-update.zip"
            Alias = 'acecode-linux-arm64-update.zip'
            Kind = 'linux'
            ContentType = 'application/zip'
            Arch = 'arm64'
        },
        [pscustomobject]@{
            Target = '' # Installer downloads must not enter the self-updater manifest.
            Source = "ACECode-$ReleaseVersion-macos-x64.pkg"
            Versioned = "ACECode-$ReleaseVersion-macos-x64.pkg"
            Alias = 'ACECode-macos-x64.pkg'
            Kind = 'macos-pkg'
            ContentType = 'application/vnd.apple.installer+xml'
            Arch = 'x64'
        },
        [pscustomobject]@{
            Target = ''
            Source = "ACECode-$ReleaseVersion-macos-arm64.pkg"
            Versioned = "ACECode-$ReleaseVersion-macos-arm64.pkg"
            Alias = 'ACECode-macos-arm64.pkg'
            Kind = 'macos-pkg'
            ContentType = 'application/vnd.apple.installer+xml'
            Arch = 'arm64'
        }
    )
}

function New-SafeTempDirectory {
    param([Parameter(Mandatory = $true)][string]$Prefix)

    $path = Join-Path ([System.IO.Path]::GetTempPath()) (
        $Prefix + [guid]::NewGuid().ToString('N'))
    [System.IO.Directory]::CreateDirectory($path) | Out-Null
    return $path
}

function Remove-SafeTempDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $tempRoot = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::GetTempPath()).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith(
            $tempRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a non-temporary directory: $resolved"
    }
    [System.IO.Directory]::Delete($resolved, $true)
}

function Wait-GitHubReleaseAssets {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Tag,
        [Parameter(Mandatory = $true)][string[]]$ExpectedNames,
        [Parameter(Mandatory = $true)][int]$TimeoutMinutes
    )

    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw 'GitHub CLI (gh) is required to mirror a published release.'
    }

    $deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)
    $lastState = 'release not found'
    while ([DateTime]::UtcNow -lt $deadline) {
        # gh 把 "release not found" 写到 stderr。脚本顶部的
        # $ErrorActionPreference = 'Stop' 会把 2>&1 合并进来的 stderr 变成
        # 终止错误,于是 tag 刚推、Release 还没被 CI 创建时,这个本来就该
        # 继续轮询的分支在第一圈就抛出去了。局部放开,让下面的
        # $LASTEXITCODE 判断自己决定要不要继续等。
        $previousEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = & gh release view $Tag --repo $Repository `
                --json tagName,isDraft,isPrerelease,publishedAt,url,assets 2>&1
        } finally {
            $ErrorActionPreference = $previousEap
        }
        if ($LASTEXITCODE -eq 0) {
            try {
                $release = (($output | Out-String).Trim()) | ConvertFrom-Json
                if ($release.isDraft) {
                    $lastState = 'release is still a draft'
                } elseif ($release.isPrerelease) {
                    throw "Stable release $Tag is marked as a GitHub prerelease."
                } else {
                    $names = @($release.assets | ForEach-Object { [string]$_.name })
                    $missing = @($ExpectedNames | Where-Object { $names -notcontains $_ })
                    if ($missing.Count -eq 0) {
                        return $release
                    }
                    $lastState = 'missing assets: ' + ($missing -join ', ')
                }
            } catch {
                if ($_.Exception.Message -like 'Stable release*') {
                    throw
                }
                $lastState = 'release metadata is not ready: ' + $_.Exception.Message
            }
        } else {
            $lastState = (($output | Out-String).Trim())
        }

        Write-Host "Waiting for GitHub $Tag assets ($lastState)"
        Start-Sleep -Seconds 15
    }

    throw "Timed out after $TimeoutMinutes minutes waiting for GitHub ${Tag}: $lastState"
}

function Download-GitHubReleaseAssets {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Tag,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $arguments = @('release', 'download', $Tag, '--repo', $Repository,
        '--dir', $Destination)
    foreach ($name in $Names) {
        $arguments += @('--pattern', $name)
    }
    Invoke-Native -FilePath gh -Arguments $arguments
}

function Read-ChecksumMap {
    param([Parameter(Mandatory = $true)][string]$Path)

    $map = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line -match '^([0-9a-f]{64})  (.+)$') {
            $map[$Matches[2]] = $Matches[1]
        }
    }
    return $map
}

function Get-ZipEntryMode {
    param([Parameter(Mandatory = $true)]$Entry)

    $raw = [System.BitConverter]::ToUInt32(
        [System.BitConverter]::GetBytes([int]$Entry.ExternalAttributes), 0)
    return (($raw -shr 16) -band 0xffff)
}

function Test-ArchiveContents {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Definition,
        [Parameter(Mandatory = $true)][string]$ReleaseVersion
    )

    if ($Definition.Kind -eq 'macos-pkg') {
        # Signed flat installers are XAR archives, not ZIPs. Their exact bytes
        # are verified against the release checksum and GitHub digest below;
        # signing/notarization validation remains owned by the macOS CI job.
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            $magic = New-Object byte[] 4
            if ($stream.Length -lt 28 -or $stream.Read($magic, 0, 4) -ne 4 -or
                [System.Text.Encoding]::ASCII.GetString($magic) -cne 'xar!') {
                throw "$($Definition.Source) is not a flat macOS PKG (XAR archive)."
            }
        } finally {
            $stream.Dispose()
        }
        return
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $byName = @{}
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName -match '\\') {
                throw "Archive entry uses a backslash: $($entry.FullName)"
            }
            if ($entry.FullName -match '(^|/)ace-browser-(host|bridge)') {
                throw "Retired browser artifact found in $($Definition.Source): $($entry.FullName)"
            }
            $byName[$entry.FullName] = $entry
        }

        $required = @()
        if ($Definition.Kind -eq 'windows') {
            $required = @(
                'acecode.exe',
                'acecode-desktop.exe',
                'share/acecode/models_dev/api.json',
                'share/acecode/models_dev/MANIFEST.json'
            )
            # Computer Use first ships in 0.9.22. Keep older-release mirror
            # repair possible without weakening the new Windows runtime gate.
            if ([version]$ReleaseVersion -ge [version]'0.9.22') {
                $required += 'acecode-computer-use.exe'
            }
        } elseif ($Definition.Kind -eq 'macos') {
            $compactMacLayout = [version]$ReleaseVersion -ge [version]'1.0.4'
            $resourceRoot = if ($compactMacLayout) { 'ACECode.app/Contents/Resources/share/acecode' } else { 'share/acecode' }
            $required = @(
                'ACECode.app/Contents/MacOS/ACECode',
                'ACECode.app/Contents/MacOS/acecode-daemon',
                'ACECode.app/Contents/Info.plist',
                'acecode',
                "$resourceRoot/models_dev/api.json",
                "$resourceRoot/models_dev/MANIFEST.json"
            )
            if ($compactMacLayout) {
                $required += @('ACECode.app/Contents/MacOS/acecode-cli',
                    "$resourceRoot/models_dev/LICENSE", "$resourceRoot/seed/seed.version",
                    "$resourceRoot/seed/MANIFEST.json")
            }
        } elseif ($Definition.Kind -eq 'linux') {
            $root = "acecode-linux-$($Definition.Arch)"
            $required = @(
                "$root/acecode",
                "$root/acecode-desktop",
                "$root/share/acecode/models_dev/api.json",
                "$root/share/acecode/models_dev/LICENSE",
                "$root/share/acecode/models_dev/MANIFEST.json"
            )
            $topLevels = @($archive.Entries |
                ForEach-Object { ($_.FullName -split '/')[0] } |
                Where-Object { $_ } |
                Sort-Object -Unique)
            if ($topLevels.Count -ne 1 -or $topLevels[0] -ne $root) {
                throw "$($Definition.Source) must contain exactly one top-level directory named $root."
            }
        }

        foreach ($name in $required) {
            if (-not $byName.ContainsKey($name)) {
                throw "$($Definition.Source) is missing required entry: $name"
            }
            if ($name -eq 'acecode-computer-use.exe' -and $byName[$name].Length -eq 0) {
                throw "$($Definition.Source) contains an empty Computer Use runtime."
            }
        }

        if ($Definition.Kind -eq 'macos') {
            if ($compactMacLayout) {
                foreach ($name in $byName.Keys) {
                    if ($name -match '^(share|channels)(/|$)|^acecode-computer-use(/|$)') {
                        throw "$($Definition.Source) contains duplicated root resources: $name"
                    }
                }
                if ($byName['acecode'].Length -gt 262144) {
                    throw "$($Definition.Source) CLI launcher exceeds 256 KiB."
                }
                $launcherHashes = @()
                foreach ($name in @('acecode', 'ACECode.app/Contents/MacOS/acecode-cli')) {
                    $stream = $byName[$name].Open()
                    $digest = [System.Security.Cryptography.SHA256]::Create()
                    try {
                        $launcherHashes += [BitConverter]::ToString($digest.ComputeHash($stream))
                    } finally {
                        $digest.Dispose()
                        $stream.Dispose()
                    }
                }
                if ($launcherHashes[0] -cne $launcherHashes[1]) {
                    throw "$($Definition.Source) root CLI launcher differs from signed app launcher."
                }
                foreach ($name in @('acecode', 'ACECode.app/Contents/MacOS/acecode-cli',
                        'ACECode.app/Contents/MacOS/ACECode', 'ACECode.app/Contents/MacOS/acecode-daemon')) {
                    if (((Get-ZipEntryMode -Entry $byName[$name]) -band 0x49) -ne 0x49) {
                        throw "$($Definition.Source) does not preserve executable mode for $name."
                    }
                }
            }
            $stream = $byName['ACECode.app/Contents/Info.plist'].Open()
            try {
                $reader = New-Object System.IO.StreamReader($stream)
                try {
                    $plist = $reader.ReadToEnd()
                } finally {
                    $reader.Dispose()
                }
            } finally {
                $stream.Dispose()
            }
            $pattern = '<key>CFBundleShortVersionString</key>\s*<string>' +
                [regex]::Escape($ReleaseVersion) + '</string>'
            if ($plist -notmatch $pattern) {
                throw "$($Definition.Source) Info.plist does not report $ReleaseVersion."
            }
        }

        if ($Definition.Kind -eq 'linux') {
            $root = "acecode-linux-$($Definition.Arch)"
            foreach ($name in @("$root/acecode", "$root/acecode-desktop")) {
                $mode = Get-ZipEntryMode -Entry $byName[$name]
                if (($mode -band 0x49) -ne 0x49) {
                    throw "$($Definition.Source) does not preserve executable mode for $name."
                }
            }
        }
    } finally {
        $archive.Dispose()
    }
}

function Compare-AceVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Left,
        [Parameter(Mandatory = $true)][string]$Right
    )

    $pattern = '^(\d+)\.(\d+)\.(\d+)(?:-(.+))?$'
    $leftMatch = [regex]::Match($Left, $pattern)
    $rightMatch = [regex]::Match($Right, $pattern)
    if (-not $leftMatch.Success -or -not $rightMatch.Success) {
        throw "Cannot compare non-semantic versions '$Left' and '$Right'."
    }
    for ($i = 1; $i -le 3; ++$i) {
        $leftPart = [int]$leftMatch.Groups[$i].Value
        $rightPart = [int]$rightMatch.Groups[$i].Value
        if ($leftPart -lt $rightPart) { return -1 }
        if ($leftPart -gt $rightPart) { return 1 }
    }

    $leftPre = $leftMatch.Groups[4].Value
    $rightPre = $rightMatch.Groups[4].Value
    if (-not $leftPre -and -not $rightPre) { return 0 }
    if (-not $leftPre) { return 1 }
    if (-not $rightPre) { return -1 }
    if ($leftPre -match '^pre\.(\d+)$' -and $rightPre -match '^pre\.(\d+)$') {
        $leftNumber = [int]([regex]::Match($leftPre, '\d+$').Value)
        $rightNumber = [int]([regex]::Match($rightPre, '\d+$').Value)
        if ($leftNumber -lt $rightNumber) { return -1 }
        if ($leftNumber -gt $rightNumber) { return 1 }
        return 0
    }
    return [System.StringComparer]::OrdinalIgnoreCase.Compare($leftPre, $rightPre)
}

function New-ManifestCandidate {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$ReleaseVersion,
        [Parameter(Mandatory = $true)][string]$ReleasePublishedAt,
        [Parameter(Mandatory = $true)][string]$ReleaseNotes,
        [Parameter(Mandatory = $true)][object[]]$Packages,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    $oldReleases = @()
    $latest = $ReleaseVersion
    if (Test-Path -LiteralPath $ManifestPath) {
        $oldManifest = [System.IO.File]::ReadAllText($ManifestPath) | ConvertFrom-Json
        if ([int]$oldManifest.schema_version -ne 1) {
            throw "Unsupported aceupdate.json schema: $($oldManifest.schema_version)"
        }
        if ($oldManifest.latest) {
            $latest = [string]$oldManifest.latest
            if ((Compare-AceVersion -Left $ReleaseVersion -Right $latest) -gt 0) {
                $latest = $ReleaseVersion
            }
        }
        $oldReleases = @($oldManifest.releases |
            Where-Object { [string]$_.version -ne $ReleaseVersion })
    }

    $newRelease = [ordered]@{
        version = $ReleaseVersion
        published_at = $ReleasePublishedAt
        notes = $ReleaseNotes
        packages = $Packages
    }

    $orderedReleases = New-Object System.Collections.ArrayList
    $inserted = $false
    foreach ($release in $oldReleases) {
        if (-not $inserted) {
            try {
                if ((Compare-AceVersion -Left $ReleaseVersion -Right ([string]$release.version)) -gt 0) {
                    [void]$orderedReleases.Add($newRelease)
                    $inserted = $true
                }
            } catch {
                # Keep non-semantic legacy records after the new release.
                [void]$orderedReleases.Add($newRelease)
                $inserted = $true
            }
        }
        [void]$orderedReleases.Add($release)
    }
    if (-not $inserted) {
        [void]$orderedReleases.Add($newRelease)
    }

    $manifest = [ordered]@{
        schema_version = 1
        latest = $latest
        releases = @($orderedReleases)
    }
    Write-Utf8NoBom -Path $DestinationPath -Text (
        ($manifest | ConvertTo-Json -Depth 20) + [Environment]::NewLine)

    $check = [System.IO.File]::ReadAllText($DestinationPath) | ConvertFrom-Json
    $matches = @($check.releases | Where-Object { $_.version -eq $ReleaseVersion })
    if ($matches.Count -ne 1 -or @($matches[0].packages).Count -ne 6) {
        throw 'Generated manifest does not contain exactly one six-platform release entry.'
    }
    return [string]$check.latest
}

function Copy-VerifiedAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][UInt64]$ExpectedSize,
        [Parameter(Mandatory = $true)][string]$ExpectedSha,
        [Parameter(Mandatory = $true)][string]$Token
    )

    $parent = Split-Path -Parent $Destination
    $parentResolved = (Resolve-Path -LiteralPath $parent).Path
    $destinationFull = [System.IO.Path]::GetFullPath($Destination)
    $prefix = $parentResolved.TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $destinationFull.StartsWith(
            $prefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe update destination: $destinationFull"
    }

    $incoming = $destinationFull + ".incoming-$Token"
    if (Test-Path -LiteralPath $incoming) {
        throw "Unexpected incoming deployment file: $incoming"
    }
    try {
        Copy-Item -LiteralPath $Source -Destination $incoming
        $item = Get-Item -LiteralPath $incoming
        $sha = (Get-FileHash -LiteralPath $incoming -Algorithm SHA256).Hash.ToLowerInvariant()
        if ([UInt64]$item.Length -ne $ExpectedSize -or $sha -ne $ExpectedSha) {
            throw "Staged update file does not match its expected size/hash: $Destination"
        }
        Move-Item -LiteralPath $incoming -Destination $destinationFull -Force
    } finally {
        if (Test-Path -LiteralPath $incoming) {
            [System.IO.File]::Delete($incoming)
        }
    }
}

function Test-PublicManifestMatches {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$ReleaseVersion,
        [Parameter(Mandatory = $true)][string]$ExpectedLatest,
        [Parameter(Mandatory = $true)][object[]]$ExpectedPackages,
        [Parameter(Mandatory = $true)][string]$ExpectedNotes
    )

    if ($Manifest.schema_version -ne 1 -or
        ([string]$Manifest.latest) -cne $ExpectedLatest -or
        $ExpectedPackages.Count -ne 6) { return $false }
    $release = @($Manifest.releases | Where-Object { ([string]$_.version) -ceq $ReleaseVersion })
    if ($release.Count -ne 1 -or ([string]$release[0].notes) -cne $ExpectedNotes -or
        @($release[0].packages).Count -ne $ExpectedPackages.Count) { return $false }

    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
    foreach ($expected in $ExpectedPackages) {
        $target = [string]$expected.target
        if ([string]::IsNullOrWhiteSpace($target) -or -not $seen.Add($target)) { return $false }
        $actual = @($release[0].packages | Where-Object { ([string]$_.target) -ceq $target })
        if ($actual.Count -ne 1 -or
            ([string]$actual[0].file) -cne ([string]$expected.file) -or
            ([string]$actual[0].sha256) -cne ([string]$expected.sha256)) { return $false }
        [UInt64]$actualSize = 0
        if (-not [UInt64]::TryParse([string]$actual[0].size, [ref]$actualSize) -or
            $actualSize -ne [UInt64]$expected.size) { return $false }
    }
    return $true
}

function Get-PublicManifest {
    param(
        [Parameter(Mandatory = $true)][string]$BaseUrl,
        [Parameter(Mandatory = $true)][string]$ReleaseVersion,
        [Parameter(Mandatory = $true)][string]$ExpectedLatest,
        [Parameter(Mandatory = $true)][object[]]$ExpectedPackages,
        [Parameter(Mandatory = $true)][string]$ExpectedNotes
    )

    $base = $BaseUrl.TrimEnd('/') + '/'
    $lastError = ''
    for ($attempt = 1; $attempt -le 10; ++$attempt) {
        $download = Join-Path ([System.IO.Path]::GetTempPath()) (
            'acecode-public-manifest-' + [guid]::NewGuid().ToString('N') + '.json')
        try {
            Invoke-WebRequest `
                -UseBasicParsing `
                -Uri ($base + 'aceupdate.json') `
                -TimeoutSec 30 `
                -OutFile $download
            $manifest = [System.IO.File]::ReadAllText(
                $download,
                [System.Text.Encoding]::UTF8) | ConvertFrom-Json
            if (Test-PublicManifestMatches -Manifest $manifest `
                    -ReleaseVersion $ReleaseVersion -ExpectedLatest $ExpectedLatest `
                    -ExpectedPackages $ExpectedPackages -ExpectedNotes $ExpectedNotes) {
                return $manifest
            }
            $lastError = 'public manifest has not reached the expected revision'
        } catch {
            $lastError = $_.Exception.Message
        } finally {
            if (Test-Path -LiteralPath $download) {
                [System.IO.File]::Delete($download)
            }
        }
        Start-Sleep -Seconds 2
    }
    throw "Public aceupdate.json verification failed: $lastError"
}

function Test-PublicFile {
    param(
        [Parameter(Mandatory = $true)][string]$BaseUrl,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][UInt64]$ExpectedSize,
        [Parameter(Mandatory = $true)][string]$ExpectedSha,
        [Parameter(Mandatory = $true)][string]$ExpectedContentType,
        [Parameter(Mandatory = $true)][string]$DownloadDirectory
    )

    $url = $BaseUrl.TrimEnd('/') + '/' + $Name
    $head = Invoke-WebRequest -UseBasicParsing -Method Head -Uri $url -TimeoutSec 30
    if ([int]$head.StatusCode -ne 200) {
        throw "Public file returned HTTP $($head.StatusCode): $url"
    }
    $contentType = [string]$head.Headers['Content-Type']
    if (-not $contentType.StartsWith(
            $ExpectedContentType,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected Content-Type '$contentType' for $url"
    }
    $contentLengthValues = @($head.Headers['Content-Length'] |
        ForEach-Object { ([string]$_).Trim() } |
        Where-Object { $_ } |
        Sort-Object -Unique)
    [UInt64]$contentLength = 0
    if ($contentLengthValues.Count -ne 1 -or
        -not [UInt64]::TryParse(
            $contentLengthValues[0],
            [ref]$contentLength)) {
        throw "Invalid Content-Length for $url"
    }
    if ($contentLength -ne $ExpectedSize) {
        throw "Unexpected Content-Length for $url"
    }

    $download = Join-Path $DownloadDirectory ([guid]::NewGuid().ToString('N') + '.download')
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 180 -OutFile $download
        $item = Get-Item -LiteralPath $download
        $sha = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
        if ([UInt64]$item.Length -ne $ExpectedSize -or $sha -ne $ExpectedSha) {
            throw "Public file body does not match expected size/hash: $url"
        }
    } finally {
        if (Test-Path -LiteralPath $download) {
            [System.IO.File]::Delete($download)
        }
    }
}

$UpgradeTip = $UpgradeTip.Trim()
if ([string]::IsNullOrWhiteSpace($UpgradeTip)) {
    throw '-UpgradeTip must contain customer-facing release notes.'
}
if ([string]::IsNullOrWhiteSpace($RemoteBaseUrl) -and
    [string]::IsNullOrWhiteSpace($AssetDirectory)) {
    throw 'Production GitHub mirroring requires -RemoteBaseUrl verification.'
}

$definitions = @(Get-AssetDefinitions -ReleaseVersion $Version)
$expectedNames = @($definitions | ForEach-Object { $_.Source }) + 'SHA256SUMS.txt'
$workRoot = New-SafeTempDirectory -Prefix "acecode-aupdate-$Version-"
$token = [guid]::NewGuid().ToString('N')
$manifestChanged = $false
$manifestPath = $null
$originalManifestBytes = $null
$originalManifestExisted = $false

try {
    $release = $null
    if ([string]::IsNullOrWhiteSpace($AssetDirectory)) {
        $release = Wait-GitHubReleaseAssets `
            -Repository $RepoSlug `
            -Tag "v$Version" `
            -ExpectedNames $expectedNames `
            -TimeoutMinutes $WaitMinutes
        $assetRoot = Join-Path $workRoot 'assets'
        [System.IO.Directory]::CreateDirectory($assetRoot) | Out-Null
        Download-GitHubReleaseAssets `
            -Repository $RepoSlug `
            -Tag "v$Version" `
            -Names $expectedNames `
            -Destination $assetRoot
        $PublishedAt = ([DateTime]$release.publishedAt).ToUniversalTime().ToString(
            'yyyy-MM-ddTHH:mm:ssZ')
    } else {
        $assetRoot = (Resolve-Path -LiteralPath $AssetDirectory).Path
        if ([string]::IsNullOrWhiteSpace($PublishedAt)) {
            $PublishedAt = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        }
    }

    $checksumPath = Join-Path $assetRoot 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf)) {
        throw 'GitHub Release is missing SHA256SUMS.txt.'
    }
    $checksumMap = Read-ChecksumMap -Path $checksumPath

    foreach ($definition in $definitions) {
        $sourcePath = Join-Path $assetRoot $definition.Source
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "GitHub Release is missing required release asset: $($definition.Source)"
        }
        if (-not $checksumMap.ContainsKey($definition.Source)) {
            throw "SHA256SUMS.txt is missing $($definition.Source)."
        }
        $item = Get-Item -LiteralPath $sourcePath
        $sha = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($sha -ne $checksumMap[$definition.Source]) {
            throw "SHA256SUMS.txt mismatch for $($definition.Source)."
        }
        if ($null -ne $release) {
            $metadata = @($release.assets |
                Where-Object { $_.name -eq $definition.Source }) |
                Select-Object -First 1
            if ($null -eq $metadata) {
                throw "GitHub metadata is missing $($definition.Source)."
            }
            if ([UInt64]$metadata.size -ne [UInt64]$item.Length) {
                throw "GitHub size metadata mismatch for $($definition.Source)."
            }
            $digest = [string]$metadata.digest
            if ($digest -and $digest -ne "sha256:$sha") {
                throw "GitHub digest metadata mismatch for $($definition.Source)."
            }
        }
        Test-ArchiveContents `
            -Path $sourcePath `
            -Definition $definition `
            -ReleaseVersion $Version
        Add-Member -InputObject $definition -NotePropertyName Size `
            -NotePropertyValue ([UInt64]$item.Length)
        Add-Member -InputObject $definition -NotePropertyName Sha256 `
            -NotePropertyValue $sha
    }

    $UpdateDir = [System.IO.Path]::GetFullPath($UpdateDir)
    [System.IO.Directory]::CreateDirectory($UpdateDir) | Out-Null
    $UpdateDir = (Resolve-Path -LiteralPath $UpdateDir).Path
    $webConfig = Join-Path $UpdateDir 'web.config'
    if (-not (Test-Path -LiteralPath $webConfig) -or
        [System.IO.File]::ReadAllText($webConfig) -notmatch 'mimeType="application/zip"') {
        throw "Update server web.config must map .zip to application/zip: $webConfig"
    }
    $webConfigXml = [xml][System.IO.File]::ReadAllText($webConfig)
    $pkgMapping = $webConfigXml.SelectSingleNode(
        '/configuration/system.webServer/staticContent/mimeMap[@fileExtension=".pkg"]')
    if ($null -eq $pkgMapping -or
        $pkgMapping.GetAttribute('mimeType') -ne 'application/vnd.apple.installer+xml') {
        throw "Update server web.config must map .pkg to application/vnd.apple.installer+xml: $webConfig"
    }

    foreach ($definition in $definitions) {
        $sourcePath = Join-Path $assetRoot $definition.Source
        Copy-VerifiedAtomic `
            -Source $sourcePath `
            -Destination (Join-Path $UpdateDir $definition.Versioned) `
            -ExpectedSize $definition.Size `
            -ExpectedSha $definition.Sha256 `
            -Token $token
    }
    foreach ($definition in $definitions) {
        $sourcePath = Join-Path $assetRoot $definition.Source
        Copy-VerifiedAtomic `
            -Source $sourcePath `
            -Destination (Join-Path $UpdateDir $definition.Alias) `
            -ExpectedSize $definition.Size `
            -ExpectedSha $definition.Sha256 `
            -Token $token
    }

    $checksumItem = Get-Item -LiteralPath $checksumPath
    $checksumSha = (Get-FileHash -LiteralPath $checksumPath -Algorithm SHA256).Hash.ToLowerInvariant()
    foreach ($name in @("SHA256SUMS-$Version.txt", 'SHA256SUMS.txt')) {
        Copy-VerifiedAtomic `
            -Source $checksumPath `
            -Destination (Join-Path $UpdateDir $name) `
            -ExpectedSize ([UInt64]$checksumItem.Length) `
            -ExpectedSha $checksumSha `
            -Token $token
    }

    $packages = @($definitions | Where-Object { $_.Target } | ForEach-Object {
        [ordered]@{
            target = $_.Target
            file = $_.Versioned
            sha256 = $_.Sha256
            size = $_.Size
        }
    })
    $manifestPath = Join-Path $UpdateDir 'aceupdate.json'
    $candidatePath = Join-Path $workRoot 'aceupdate.candidate.json'
    $expectedLatest = New-ManifestCandidate `
        -ManifestPath $manifestPath `
        -ReleaseVersion $Version `
        -ReleasePublishedAt $PublishedAt `
        -ReleaseNotes $UpgradeTip `
        -Packages $packages `
        -DestinationPath $candidatePath

    $originalManifestExisted = Test-Path -LiteralPath $manifestPath
    if ($originalManifestExisted) {
        $originalManifestBytes = [System.IO.File]::ReadAllBytes($manifestPath)
    }
    $incomingManifest = Join-Path $UpdateDir ".aceupdate.json.incoming-$token"
    Copy-Item -LiteralPath $candidatePath -Destination $incomingManifest
    if ($originalManifestExisted) {
        $backupPath = Join-Path (Split-Path -Parent $UpdateDir) `
            ".aceupdate-backup-$token.json"
        [System.IO.File]::Replace(
            $incomingManifest,
            $manifestPath,
            $backupPath,
            $true)
        [System.IO.File]::Delete($backupPath)
    } else {
        Move-Item -LiteralPath $incomingManifest -Destination $manifestPath
    }
    $manifestChanged = $true

    if (-not [string]::IsNullOrWhiteSpace($RemoteBaseUrl)) {
        $publicManifest = Get-PublicManifest `
            -BaseUrl $RemoteBaseUrl `
            -ReleaseVersion $Version `
            -ExpectedLatest $expectedLatest `
            -ExpectedPackages $packages `
            -ExpectedNotes $UpgradeTip

        $publicDownloads = Join-Path $workRoot 'public-downloads'
        [System.IO.Directory]::CreateDirectory($publicDownloads) | Out-Null
        foreach ($definition in $definitions) {
            foreach ($name in @($definition.Versioned, $definition.Alias)) {
                Test-PublicFile `
                    -BaseUrl $RemoteBaseUrl `
                    -Name $name `
                    -ExpectedSize $definition.Size `
                    -ExpectedSha $definition.Sha256 `
                    -ExpectedContentType $definition.ContentType `
                    -DownloadDirectory $publicDownloads
            }
        }
        foreach ($name in @("SHA256SUMS-$Version.txt", 'SHA256SUMS.txt')) {
            Test-PublicFile `
                -BaseUrl $RemoteBaseUrl `
                -Name $name `
                -ExpectedSize ([UInt64]$checksumItem.Length) `
                -ExpectedSha $checksumSha `
                -ExpectedContentType 'text/plain' `
                -DownloadDirectory $publicDownloads
        }
    }

    Write-Host "GitHub release mirror complete: v$Version (six updater ZIPs and two macOS PKGs)"
    Write-Host "Manifest latest: $expectedLatest"
    foreach ($definition in $definitions) {
        Write-Host (
            "$($definition.Kind)/$($definition.Arch): $($definition.Versioned) " +
            "($($definition.Size) bytes, $($definition.Sha256))")
    }
} catch {
    if ($manifestChanged -and $null -ne $manifestPath) {
        if ($originalManifestExisted) {
            $restoreIncoming = Join-Path $UpdateDir ".aceupdate.json.restore-$token"
            [System.IO.File]::WriteAllBytes($restoreIncoming, $originalManifestBytes)
            $discard = Join-Path (Split-Path -Parent $UpdateDir) `
                ".aceupdate-discard-$token.json"
            [System.IO.File]::Replace(
                $restoreIncoming,
                $manifestPath,
                $discard,
                $true)
            [System.IO.File]::Delete($discard)
        } elseif (Test-Path -LiteralPath $manifestPath) {
            [System.IO.File]::Delete($manifestPath)
        }
    }
    throw
} finally {
    Remove-SafeTempDirectory -Path $workRoot
}
