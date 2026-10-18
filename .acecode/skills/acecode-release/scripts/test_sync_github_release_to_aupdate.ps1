[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
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

# Import only the exact production function under test, without starting a
# release, touching Git, or contacting the public update server.
function Get-ScriptFunction {
    param([string]$Path, [string]$Name)
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if ($errors.Count -gt 0) { throw "Script parse failed: $Path" }
    $function = $ast.Find({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $Name
    }, $true)
    if ($null -eq $function) { throw "Missing function ${Name}: $Path" }
    return [scriptblock]::Create($function.Extent.Text)
}

function New-TestZip {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Entries,
        [string[]]$ExecutableEntries = @()
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::Open(
        $Path,
        [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $Entries.Keys) {
            $entry = $archive.CreateEntry(
                [string]$name,
                [System.IO.Compression.CompressionLevel]::Optimal)
            if ($ExecutableEntries -contains $name) {
                $unixAttributes = [Convert]::ToUInt32('81ed0000', 16)
                $entry.ExternalAttributes = [System.BitConverter]::ToInt32(
                    [System.BitConverter]::GetBytes($unixAttributes), 0)
            }
            $stream = $entry.Open()
            try {
                $writer = New-Object System.IO.StreamWriter($stream)
                try {
                    $writer.Write([string]$Entries[$name])
                } finally {
                    $writer.Dispose()
                }
            } finally {
                $stream.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
    }
}

function New-FixtureAssets {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$Version
    )

    [System.IO.Directory]::CreateDirectory($Directory) | Out-Null
    foreach ($arch in @('x64', 'arm64')) {
        New-TestZip `
            -Path (Join-Path $Directory "acecode-windows-$arch.zip") `
            -Entries ([ordered]@{
                'acecode.exe' = "windows-$arch-$Version-cli"
                'acecode-desktop.exe' = "windows-$arch-$Version-desktop"
                'acecode-computer-use.exe' = "windows-$arch-$Version-computer-use"
                'share/acecode/models_dev/api.json' = '{}'
                'share/acecode/models_dev/MANIFEST.json' = '{}'
            })

        $plist = @"
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundleShortVersionString</key><string>$Version</string>
</dict></plist>
"@
        New-TestZip `
            -Path (Join-Path $Directory "ACECode-$Version-macos-$arch-update.zip") `
            -Entries ([ordered]@{
                'ACECode.app/Contents/MacOS/ACECode' = "mac-$arch-app"
                'ACECode.app/Contents/MacOS/acecode-daemon' = "mac-$arch-daemon"
                'ACECode.app/Contents/Info.plist' = $plist
                'acecode' = "mac-$arch-cli"
                'ACECode.app/Contents/MacOS/acecode-cli' = "mac-$arch-cli"
                'ACECode.app/Contents/Resources/share/acecode/models_dev/api.json' = '{}'
                'ACECode.app/Contents/Resources/share/acecode/models_dev/MANIFEST.json' = '{}'
                'ACECode.app/Contents/Resources/share/acecode/models_dev/LICENSE' = 'fixture'
                'ACECode.app/Contents/Resources/share/acecode/seed/seed.version' = '2026-10-10.1'
                'ACECode.app/Contents/Resources/share/acecode/seed/MANIFEST.json' = '{}'
            }) `
            -ExecutableEntries @('acecode', 'ACECode.app/Contents/MacOS/acecode-cli',
                'ACECode.app/Contents/MacOS/ACECode', 'ACECode.app/Contents/MacOS/acecode-daemon')

        # Minimal XAR header fixture; macOS CI owns signing/notarization checks.
        [System.IO.File]::WriteAllBytes(
            (Join-Path $Directory "ACECode-$Version-macos-$arch.pkg"),
            [System.Text.Encoding]::ASCII.GetBytes(('xar!' + ('0' * 24) + $arch)))

        $root = "acecode-linux-$arch"
        $linuxEntries = [ordered]@{
            "$root/acecode" = "linux-$arch-$Version-cli"
            "$root/acecode-desktop" = "linux-$arch-$Version-desktop"
            "$root/share/acecode/models_dev/api.json" = '{}'
            "$root/share/acecode/models_dev/LICENSE" = 'fixture'
            "$root/share/acecode/models_dev/MANIFEST.json" = '{}'
        }
        New-TestZip `
            -Path (Join-Path $Directory "acecode-$Version-linux-$arch-update.zip") `
            -Entries $linuxEntries `
            -ExecutableEntries @("$root/acecode", "$root/acecode-desktop")
    }

    $lines = @()
    Get-ChildItem -LiteralPath $Directory -File |
        Where-Object { $_.Extension -in @('.zip', '.pkg') } |
        Sort-Object Name |
        ForEach-Object {
            $sha = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $lines += "$sha  $($_.Name)"
        }
    Write-Utf8NoBom `
        -Path (Join-Path $Directory 'SHA256SUMS.txt') `
        -Text (($lines -join "`n") + "`n")
}

function Initialize-UpdateDirectory {
    param([Parameter(Mandatory = $true)][string]$Directory)

    [System.IO.Directory]::CreateDirectory($Directory) | Out-Null
    Write-Utf8NoBom -Path (Join-Path $Directory 'web.config') -Text @'
<?xml version="1.0" encoding="utf-8"?>
<configuration><system.webServer><staticContent>
<remove fileExtension=".zip" />
<mimeMap fileExtension=".zip" mimeType="application/zip" />
<remove fileExtension=".pkg" />
<mimeMap fileExtension=".pkg" mimeType="application/vnd.apple.installer+xml" />
</staticContent></system.webServer></configuration>
'@
    Write-Utf8NoBom -Path (Join-Path $Directory 'aceupdate.json') -Text @'
{
  "schema_version": 1,
  "latest": "1.2.4-pre.1",
  "releases": [
    {
      "version": "1.2.4-pre.1",
      "published_at": "2026-08-29T00:00:00Z",
      "notes": "Windows-only validation.",
      "packages": [
        {
          "target": "windows-x64",
          "file": "acecode-1.2.4-pre.1-windows-x64.zip",
          "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "size": 1
        }
      ]
    },
    {
      "version": "1.2.2",
      "published_at": "2026-08-28T00:00:00Z",
      "notes": "Previous stable.",
      "packages": []
    }
  ]
}
'@
}

$scriptPath = Join-Path $PSScriptRoot 'sync_github_release_to_aupdate.ps1'
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'acecode-release-sync-test-' + [guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($testRoot) | Out-Null

try {
    . (Get-ScriptFunction -Path (Join-Path $PSScriptRoot 'publish_acecode_release.ps1') -Name 'Ensure-ZipMimeConfig')
    . (Get-ScriptFunction -Path $scriptPath -Name 'Test-PublicManifestMatches')

    $freshMime = Join-Path $testRoot 'fresh-mime'
    [System.IO.Directory]::CreateDirectory($freshMime) | Out-Null
    Ensure-ZipMimeConfig -Directory $freshMime
    $freshConfig = [xml][System.IO.File]::ReadAllText((Join-Path $freshMime 'web.config'))
    foreach ($pair in @(@('.zip', 'application/zip'), @('.pkg', 'application/vnd.apple.installer+xml'))) {
        $node = $freshConfig.SelectSingleNode("/configuration/system.webServer/staticContent/mimeMap[@fileExtension='$($pair[0])']")
        Assert-True ($null -ne $node -and $node.GetAttribute('mimeType') -ceq $pair[1]) "Fresh config must serve $($pair[0])."
    }
    $legacyMime = Join-Path $testRoot 'legacy-mime'
    [System.IO.Directory]::CreateDirectory($legacyMime) | Out-Null
    $legacyConfigPath = Join-Path $legacyMime 'web.config'
    Write-Utf8NoBom $legacyConfigPath '<configuration><system.webServer><httpProtocol><customHeaders><add name="X-Keep" value="retained" /></customHeaders></httpProtocol><staticContent><mimeMap fileExtension=".zip" mimeType="application/zip" /><mimeMap fileExtension=".pkg" mimeType="wrong/type" /><mimeMap fileExtension=".custom" mimeType="application/custom" /></staticContent></system.webServer></configuration>'
    Ensure-ZipMimeConfig -Directory $legacyMime
    Ensure-ZipMimeConfig -Directory $legacyMime
    $legacyConfig = [xml][System.IO.File]::ReadAllText($legacyConfigPath)
    Assert-True ($legacyConfig.SelectSingleNode('//add[@name="X-Keep"]').GetAttribute('value') -ceq 'retained') 'MIME repair must preserve unrelated IIS settings.'
    Assert-True ($legacyConfig.SelectSingleNode('//mimeMap[@fileExtension=".custom"]').GetAttribute('mimeType') -ceq 'application/custom') 'MIME repair must preserve unrelated MIME mappings.'
    foreach ($pair in @(@('.zip', 'application/zip'), @('.pkg', 'application/vnd.apple.installer+xml'))) {
        $nodes = @($legacyConfig.SelectNodes("//mimeMap[@fileExtension='$($pair[0])']"))
        Assert-True ($nodes.Count -eq 1 -and $nodes[0].GetAttribute('mimeType') -ceq $pair[1]) 'MIME repair must be correct and idempotent.'
        Assert-True (@($legacyConfig.SelectNodes("//remove[@fileExtension='$($pair[0])']")).Count -eq 1) 'MIME repair must override inherited IIS mappings exactly once.'
    }

    $version = '1.2.3'
    $assets = Join-Path $testRoot 'assets'
    $updateDir = Join-Path $testRoot 'aupdate'
    New-FixtureAssets -Directory $assets -Version $version
    Initialize-UpdateDirectory -Directory $updateDir

    & $scriptPath `
        -Version $version `
        -UpgradeTip 'Cross-platform updater fixture.' `
        -UpdateDir $updateDir `
        -RemoteBaseUrl '' `
        -AssetDirectory $assets `
        -PublishedAt '2026-08-29T05:29:53Z'

    $manifest = [System.IO.File]::ReadAllText(
        (Join-Path $updateDir 'aceupdate.json')) | ConvertFrom-Json
    Assert-True `
        -Condition ($manifest.latest -eq '1.2.4-pre.1') `
        -Message 'A newer Windows prerelease must remain manifest latest.'
    $release = @($manifest.releases | Where-Object { $_.version -eq $version })
    Assert-True `
        -Condition ($release.Count -eq 1) `
        -Message 'Stable fixture release must appear exactly once.'
    Assert-True `
        -Condition (@($release[0].packages).Count -eq 6) `
        -Message 'Stable release must contain all six updater targets.'
    $expectedTargets = @(
        'windows-x64',
        'windows-arm64',
        'macos-x64',
        'macos-arm64',
        'linux-x64-updater-v1',
        'linux-arm64-updater-v1'
    )
    Assert-True `
        -Condition ((@($release[0].packages.target) -join ',') -eq ($expectedTargets -join ',')) `
        -Message 'Manifest updater targets are incomplete or out of order.'

    $expectedPublic = @{
        ReleaseVersion = $version
        ExpectedLatest = [string]$manifest.latest
        ExpectedPackages = @($release[0].packages)
        ExpectedNotes = [string]$release[0].notes
    }
    Assert-True (Test-PublicManifestMatches -Manifest $manifest @expectedPublic) 'Exact public manifest must pass.'
    foreach ($field in @('target', 'file', 'sha256', 'size', 'notes', 'duplicate-target')) {
        $stale = ($manifest | ConvertTo-Json -Depth 20) | ConvertFrom-Json
        $staleRelease = @($stale.releases | Where-Object { $_.version -eq $version })[0]
        if ($field -eq 'notes') {
            $staleRelease.notes = 'Old cached notes.'
        } elseif ($field -eq 'size') {
            $staleRelease.packages[0].size = [UInt64]$staleRelease.packages[0].size + 1
        } elseif ($field -eq 'duplicate-target') {
            $staleRelease.packages[0].target = $staleRelease.packages[1].target
        } else {
            $staleRelease.packages[0].$field = 'stale-' + [string]$staleRelease.packages[0].$field
        }
        Assert-True (-not (Test-PublicManifestMatches -Manifest $stale @expectedPublic)) "Public manifest must reject stale $field with the same version and six packages."
    }
    $reordered = ($manifest | ConvertTo-Json -Depth 20) | ConvertFrom-Json
    $reorderedRelease = @($reordered.releases | Where-Object { $_.version -eq $version })[0]
    [array]::Reverse($reorderedRelease.packages)
    Assert-True (Test-PublicManifestMatches -Manifest $reordered @expectedPublic) 'Package matching must use target identity, not array order.'

    # Exercise the actual polling function: identical version/count/notes but an
    # old hash must not finish verification before the next uncached response.
    & {
        . (Get-ScriptFunction -Path $scriptPath -Name 'Get-PublicManifest')
        $script:manifestPollCount = 0
        $script:manifestPollForeverStale = $false
        $script:manifestPollFresh = ($manifest | ConvertTo-Json -Depth 20)
        $cached = $script:manifestPollFresh | ConvertFrom-Json
        (@($cached.releases | Where-Object { $_.version -eq $version })[0]).packages[0].sha256 = ('0' * 64)
        $script:manifestPollStale = $cached | ConvertTo-Json -Depth 20
        function Invoke-WebRequest {
            param([switch]$UseBasicParsing, [string]$Uri, [int]$TimeoutSec, [string]$OutFile)
            $script:manifestPollCount++
            $body = if ($script:manifestPollForeverStale -or $script:manifestPollCount -eq 1) {
                $script:manifestPollStale
            } else { $script:manifestPollFresh }
            Write-Utf8NoBom -Path $OutFile -Text $body
        }
        function Start-Sleep { param([int]$Seconds) }
        $verified = Get-PublicManifest -BaseUrl 'https://example.invalid/' @expectedPublic
        Assert-True ($script:manifestPollCount -eq 2) 'Polling must wait past the stale hash response.'
        Assert-True (Test-PublicManifestMatches -Manifest $verified @expectedPublic) 'Polling must return the verified current manifest.'
        $script:manifestPollCount = 0
        $script:manifestPollForeverStale = $true
        $failed = $false
        try { Get-PublicManifest -BaseUrl 'https://example.invalid/' @expectedPublic | Out-Null }
        catch { $failed = $_.Exception.Message -like 'Public aceupdate.json verification failed:*' }
        Assert-True ($failed -and $script:manifestPollCount -eq 10) 'Permanently stale public metadata must fail after the bounded poll.'
    }

    $pairs = @(
        @('acecode-windows-x64.zip', "acecode-$version-windows-x64.zip", 'acecode-windows-x64.zip'),
        @('acecode-windows-arm64.zip', "acecode-$version-windows-arm64.zip", 'acecode-windows-arm64.zip'),
        @("ACECode-$version-macos-x64-update.zip", "ACECode-$version-macos-x64-update.zip", 'ACECode-macos-x64-update.zip'),
        @("ACECode-$version-macos-arm64-update.zip", "ACECode-$version-macos-arm64-update.zip", 'ACECode-macos-arm64-update.zip'),
        @("acecode-$version-linux-x64-update.zip", "acecode-$version-linux-x64-update.zip", 'acecode-linux-x64-update.zip'),
        @("acecode-$version-linux-arm64-update.zip", "acecode-$version-linux-arm64-update.zip", 'acecode-linux-arm64-update.zip'),
        @("ACECode-$version-macos-x64.pkg", "ACECode-$version-macos-x64.pkg", 'ACECode-macos-x64.pkg'),
        @("ACECode-$version-macos-arm64.pkg", "ACECode-$version-macos-arm64.pkg", 'ACECode-macos-arm64.pkg')
    )
    foreach ($pair in $pairs) {
        $sourceHash = (Get-FileHash -LiteralPath (Join-Path $assets $pair[0]) -Algorithm SHA256).Hash
        foreach ($destinationName in @($pair[1], $pair[2])) {
            $destination = Join-Path $updateDir $destinationName
            Assert-True `
                -Condition (Test-Path -LiteralPath $destination -PathType Leaf) `
                -Message "Missing mirrored release asset: $destinationName"
            $destinationHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
            Assert-True `
                -Condition ($destinationHash -eq $sourceHash) `
                -Message "Mirrored release asset hash mismatch: $destinationName"
        }
    }
    Assert-True `
        -Condition (Test-Path -LiteralPath (Join-Path $updateDir "SHA256SUMS-$version.txt")) `
        -Message 'Versioned checksum file is missing.'
    Assert-True `
        -Condition (Test-Path -LiteralPath (Join-Path $updateDir 'SHA256SUMS.txt')) `
        -Message 'Latest checksum alias is missing.'

    foreach ($scenario in @('missing-updater', 'missing-x64-pkg', 'missing-arm64-pkg',
            'tampered-pkg', 'invalid-pkg', 'missing-pkg-checksum', 'missing-pkg-mime',
            'missing-windows-helper', 'empty-windows-helper',
            'duplicated-mac-resources', 'different-mac-launcher', 'oversized-mac-launcher',
            'missing-mac-resource', 'nonexecutable-mac-launcher')) {
        $negativeAssets = Join-Path $testRoot "$scenario-assets"
        Copy-Item -LiteralPath $assets -Destination $negativeAssets -Recurse
        $negativeUpdateDir = Join-Path $testRoot "$scenario-aupdate"
        Initialize-UpdateDirectory -Directory $negativeUpdateDir
        $pkgName = "ACECode-$version-macos-arm64.pkg"
        $pkgPath = Join-Path $negativeAssets $pkgName
        $checksumPath = Join-Path $negativeAssets 'SHA256SUMS.txt'
        $expectedError = ''
        if ($scenario -in @('missing-windows-helper', 'empty-windows-helper')) {
            $windowsName = 'acecode-windows-arm64.zip'
            $windowsPath = Join-Path $negativeAssets $windowsName
            $zip = [System.IO.Compression.ZipFile]::Open($windowsPath, [System.IO.Compression.ZipArchiveMode]::Update)
            try {
                $zip.GetEntry('acecode-computer-use.exe').Delete()
                if ($scenario -eq 'empty-windows-helper') {
                    $zip.CreateEntry('acecode-computer-use.exe') | Out-Null
                }
            } finally { $zip.Dispose() }
            $newSha = (Get-FileHash -LiteralPath $windowsPath -Algorithm SHA256).Hash.ToLowerInvariant()
            $checksums = [System.IO.File]::ReadAllText($checksumPath)
            $checksums = $checksums -replace ('(?m)^[0-9a-f]{64}  ' + [regex]::Escape($windowsName) + '$'), "$newSha  $windowsName"
            Write-Utf8NoBom -Path $checksumPath -Text $checksums
            $expectedError = if ($scenario -eq 'missing-windows-helper') {
                'missing required entry: acecode-computer-use.exe'
            } else { 'empty Computer Use runtime' }
        }
        switch ($scenario) {
            { $_ -in @('duplicated-mac-resources', 'different-mac-launcher', 'oversized-mac-launcher',
                    'missing-mac-resource', 'nonexecutable-mac-launcher') } {
                $macName = "ACECode-$version-macos-arm64-update.zip"
                $macPath = Join-Path $negativeAssets $macName
                $archive = [System.IO.Compression.ZipFile]::Open($macPath, [System.IO.Compression.ZipArchiveMode]::Update)
                try {
                    switch ($scenario) {
                        'duplicated-mac-resources' {
                            $null = $archive.CreateEntry('share/acecode/models_dev/api.json')
                            $expectedError = 'contains duplicated root resources'
                        }
                        'missing-mac-resource' {
                            $archive.GetEntry('ACECode.app/Contents/Resources/share/acecode/seed/seed.version').Delete()
                            $expectedError = 'is missing required entry'
                        }
                        'nonexecutable-mac-launcher' {
                            $archive.GetEntry('acecode').ExternalAttributes = 0
                            $expectedError = 'does not preserve executable mode'
                        }
                        default {
                            $archive.GetEntry('acecode').Delete()
                            $entry = $archive.CreateEntry('acecode')
                            $stream = $entry.Open()
                            try {
                                $bytes = [Text.Encoding]::UTF8.GetBytes($(if ($scenario -eq 'oversized-mac-launcher') { 'x' * 262145 } else { 'different' }))
                                $stream.Write($bytes, 0, $bytes.Length)
                            } finally { $stream.Dispose() }
                            $expectedError = if ($scenario -eq 'oversized-mac-launcher') { 'exceeds 256 KiB' } else { 'differs from signed app launcher' }
                        }
                    }
                } finally { $archive.Dispose() }
                $newSha = (Get-FileHash -LiteralPath $macPath -Algorithm SHA256).Hash.ToLowerInvariant()
                $checksums = [IO.File]::ReadAllText($checksumPath) -replace ('(?m)^[0-9a-f]{64}  ' + [regex]::Escape($macName) + '$'), "$newSha  $macName"
                Write-Utf8NoBom -Path $checksumPath -Text $checksums
            }
            'missing-updater' {
                $missingName = "ACECode-$version-macos-arm64-update.zip"
                [System.IO.File]::Delete((Join-Path $negativeAssets $missingName))
                $expectedError = "missing required release asset: $missingName"
            }
            'missing-x64-pkg' {
                $missingName = "ACECode-$version-macos-x64.pkg"
                [System.IO.File]::Delete((Join-Path $negativeAssets $missingName))
                $expectedError = "missing required release asset: $missingName"
            }
            'missing-arm64-pkg' {
                [System.IO.File]::Delete($pkgPath)
                $expectedError = "missing required release asset: $pkgName"
            }
            'tampered-pkg' {
                [System.IO.File]::AppendAllText($pkgPath, 'tampered')
                $expectedError = "SHA256SUMS.txt mismatch for $pkgName"
            }
            'invalid-pkg' {
                Write-Utf8NoBom -Path $pkgPath -Text 'This is not a macOS installer archive.'
                $newSha = (Get-FileHash -LiteralPath $pkgPath -Algorithm SHA256).Hash.ToLowerInvariant()
                $checksums = [System.IO.File]::ReadAllText($checksumPath)
                $checksums = $checksums -replace ('(?m)^[0-9a-f]{64}  ' + [regex]::Escape($pkgName) + '$'), "$newSha  $pkgName"
                Write-Utf8NoBom -Path $checksumPath -Text $checksums
                $expectedError = 'not a flat macOS PKG'
            }
            'missing-pkg-checksum' {
                $lines = [System.IO.File]::ReadAllLines($checksumPath) |
                    Where-Object { -not $_.EndsWith($pkgName) }
                Write-Utf8NoBom -Path $checksumPath -Text (($lines -join "`n") + "`n")
                $expectedError = "SHA256SUMS.txt is missing $pkgName"
            }
            'missing-pkg-mime' {
                $configPath = Join-Path $negativeUpdateDir 'web.config'
                $config = [System.IO.File]::ReadAllText($configPath) -replace '(?m)^.*fileExtension="\.pkg".*\r?\n', ''
                Write-Utf8NoBom -Path $configPath -Text $config
                $expectedError = 'must map .pkg'
            }
        }
        $before = [System.IO.File]::ReadAllBytes(
            (Join-Path $negativeUpdateDir 'aceupdate.json'))
        $failedAsExpected = $false
        try {
            & $scriptPath `
                -Version $version `
                -UpgradeTip 'This run must fail.' `
                -UpdateDir $negativeUpdateDir `
                -RemoteBaseUrl '' `
                -AssetDirectory $negativeAssets `
                -PublishedAt '2026-08-29T05:29:53Z'
        } catch {
            if ($_.Exception.Message.Contains($expectedError)) {
                $failedAsExpected = $true
            } else {
                throw
            }
        }
        Assert-True -Condition $failedAsExpected -Message "$scenario must fail the stable mirror."
        $after = [System.IO.File]::ReadAllBytes(
            (Join-Path $negativeUpdateDir 'aceupdate.json'))
        Assert-True `
            -Condition ([System.Linq.Enumerable]::SequenceEqual([byte[]]$before, [byte[]]$after)) `
            -Message "$scenario must leave aceupdate.json unchanged."
        Assert-True `
            -Condition (@(Get-ChildItem -LiteralPath $negativeUpdateDir -File).Count -eq 2) `
            -Message "$scenario must fail before deploying release files or aliases."
    }

    Write-Host 'GitHub Release to aupdate regression tests passed.'
} finally {
    $resolved = (Resolve-Path -LiteralPath $testRoot).Path
    $tempRoot = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::GetTempPath()).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith(
            $tempRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove non-temporary test directory: $resolved"
    }
    [System.IO.Directory]::Delete($resolved, $true)
}
