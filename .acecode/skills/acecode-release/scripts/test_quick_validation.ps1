[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $parent = Split-Path -Parent $Path
    if ($parent) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
    }
}

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'acecode-release-quick-test-' + [guid]::NewGuid().ToString('N'))
$repo = Join-Path $testRoot 'repo'
$updateDir = Join-Path $testRoot 'aupdate'
$releaseScript = Join-Path $PSScriptRoot 'publish_acecode_release.ps1'

try {
    New-Item -ItemType Directory -Force -Path (Join-Path $repo 'build\Release') | Out-Null

    Write-Utf8NoBom -Path (Join-Path $repo 'CMakeLists.txt') -Text @'
cmake_minimum_required(VERSION 3.20)
project(acecode VERSION 0.8.6 LANGUAGES C CXX)
'@
    Write-Utf8NoBom -Path (Join-Path $repo 'vcpkg.json') -Text @'
{
  "name": "acecode",
  "version-semver": "0.8.6"
}
'@
    Write-Utf8NoBom -Path (Join-Path $repo 'src\version.hpp.in') -Text @'
#pragma once
#define ACECODE_VERSION_MAJOR @acecode_VERSION_MAJOR@
#define ACECODE_VERSION_MINOR @acecode_VERSION_MINOR@
#define ACECODE_VERSION_PATCH @acecode_VERSION_PATCH@
#define ACECODE_VERSION "@acecode_VERSION@"
'@
    Write-Utf8NoBom -Path (Join-Path $repo 'assets\models_dev\api.json') -Text "{}`n"
    Write-Utf8NoBom -Path (Join-Path $repo 'assets\seed\MANIFEST.json') -Text "{}`n"
    foreach ($asset in @('bridge.mjs', 'protocol.mjs', 'package.json', 'package-lock.json')) {
        Write-Utf8NoBom -Path (Join-Path $repo "assets\channels\whatsapp\$asset") -Text "{}`n"
    }
    Write-Utf8NoBom -Path (Join-Path $repo '.gitignore') -Text "build/`n"
    Write-Utf8NoBom -Path (Join-Path $updateDir 'aceupdate.json') -Text @'
{
  "schema_version": 1,
  "latest": "0.8.7-pre.2",
  "releases": [
    {
      "version": "0.8.7-pre.2",
      "published_at": "2026-08-01T00:00:00Z",
      "notes": "Prior validation package.",
      "packages": [
        {
          "target": "windows-x64",
          "file": "acecode-0.8.7-pre.2-windows-x64.zip",
          "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
          "size": 1
        }
      ]
    },
    {
      "version": "0.8.6",
      "published_at": "2026-07-31T00:00:00Z",
      "notes": "Stable package.",
      "packages": []
    }
  ]
}
'@

    $fakeExe = Join-Path $repo 'build\Release\acecode.exe'
    $source = @'
using System;
using System.IO;
using System.Text.RegularExpressions;
public static class QuickValidationFakeProgram
{
    public static void Main(string[] args)
    {
        string forcedVersion = Environment.GetEnvironmentVariable("ACECODE_RELEASE_TEST_FORCE_VERSION");
        if (!String.IsNullOrEmpty(forcedVersion))
        {
            Console.WriteLine("acecode v" + forcedVersion);
            return;
        }

        string repo = Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "..", ".."));
        string template = File.ReadAllText(Path.Combine(repo, "src", "version.hpp.in"));
        string cmake = File.ReadAllText(Path.Combine(repo, "CMakeLists.txt"));
        Match fullVersion = Regex.Match(template, "#define ACECODE_VERSION \\\"([^\\\"]+)\\\"");
        Match numericVersion = Regex.Match(cmake, "project\\(acecode VERSION ([0-9]+\\.[0-9]+\\.[0-9]+) LANGUAGES C CXX\\)");
        if (!fullVersion.Success || !numericVersion.Success ||
            !fullVersion.Groups[1].Value.StartsWith(numericVersion.Groups[1].Value + "-", StringComparison.Ordinal))
        {
            Console.WriteLine("acecode vinvalid-test-version");
            return;
        }
        Console.WriteLine("acecode v" + fullVersion.Groups[1].Value);
    }
}
'@
    Add-Type -TypeDefinition $source -Language CSharp -OutputType ConsoleApplication -OutputAssembly $fakeExe
    Copy-Item -LiteralPath $fakeExe -Destination (Join-Path $repo 'build\Release\acecode-desktop.exe')
    Copy-Item -LiteralPath $fakeExe -Destination (Join-Path $repo 'build\Release\acecode-computer-use.exe')
    # Deliberately leave a stale legacy host in the build tree. Packaging must
    # follow the current Agent Browser layout and never pick it up.
    Copy-Item -LiteralPath $fakeExe -Destination (Join-Path $repo 'build\Release\ace-browser-host.exe')

    Invoke-Native git -C $repo init -q
    Invoke-Native git -C $repo config user.name 'ACECode Release Test'
    Invoke-Native git -C $repo config user.email 'acecode-release-test@example.invalid'
    Invoke-Native git -C $repo config core.autocrlf false
    Invoke-Native git -C $repo add -- CMakeLists.txt vcpkg.json src/version.hpp.in assets/models_dev/api.json assets/seed/MANIFEST.json assets/channels/whatsapp .gitignore
    Invoke-Native git -C $repo commit -q -m 'Create quick validation fixture'

    $sourcePaths = @(
        (Join-Path $repo 'CMakeLists.txt'),
        (Join-Path $repo 'vcpkg.json'),
        (Join-Path $repo 'src\version.hpp.in')
    )
    $beforeHashes = @{}
    foreach ($path in $sourcePaths) {
        $beforeHashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $headBefore = (& git -C $repo rev-parse HEAD).Trim()

    $pushWasBlocked = $false
    try {
        & $releaseScript `
            -QuickValidation `
            -Version '0.8.7-pre.3' `
            -Repo $repo -BuildDir 'build' `
            -UpdateDir $updateDir `
            -RemoteBaseUrl '' `
            -SkipBuild `
            -Push
    } catch {
        $pushWasBlocked = $_.Exception.Message -like 'Quick validation never pushes*'
    }
    Assert-True $pushWasBlocked 'Quick validation did not block -Push.'

    & $releaseScript `
        -QuickValidation `
        -Repo $repo -BuildDir 'build' `
        -UpdateDir $updateDir `
        -RemoteBaseUrl '' `
        -SkipBuild

    $expectedVersion = '0.8.7-pre.3'
    $zipPath = Join-Path $updateDir "acecode-$expectedVersion-windows-x64.zip"
    Assert-True (Test-Path -LiteralPath $zipPath) 'Quick validation Windows zip was not created.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $updateDir "ace-browser-bridge-$expectedVersion.zip"))) 'Quick validation unexpectedly created a browser extension zip.'

    $manifest = ([System.IO.File]::ReadAllText((Join-Path $updateDir 'aceupdate.json'))) | ConvertFrom-Json
    Assert-True ($manifest.latest -eq $expectedVersion) 'Quick validation manifest latest version is incorrect.'
    $release = @($manifest.releases) | Select-Object -First 1
    Assert-True ($release.version -eq $expectedVersion) 'Quick validation release version is incorrect.'
    Assert-True (@($release.packages).Count -eq 1) 'Quick validation must publish exactly one package entry.'
    Assert-True ($release.packages[0].target -eq 'windows-x64') 'Quick validation package target is not windows-x64.'
    Assert-True (@($manifest.releases | Where-Object { $_.version -eq '0.8.7-pre.2' }).Count -eq 1) 'Quick validation did not preserve the prior prerelease record.'
    Assert-True (@($manifest.releases | Where-Object { $_.version -eq '0.8.6' }).Count -eq 1) 'Quick validation did not preserve the stable release record.'

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entries = @($archive.Entries | ForEach-Object { $_.FullName })
        foreach ($required in @(
            'acecode.exe',
            'acecode-desktop.exe',
            'acecode-computer-use.exe',
            'channels/whatsapp/bridge.mjs',
            'channels/whatsapp/protocol.mjs',
            'channels/whatsapp/package.json',
            'channels/whatsapp/package-lock.json',
            'share/acecode/models_dev/api.json',
            'share/acecode/seed/MANIFEST.json')) {
            Assert-True ($entries -contains $required) "Quick validation zip is missing $required."
        }
        Assert-True (-not (@($entries | Where-Object { $_ -like 'ace-browser-*' }).Count)) 'Quick validation zip contains a legacy browser artifact.'
    } finally {
        $archive.Dispose()
    }

    foreach ($path in $sourcePaths) {
        $afterHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        Assert-True ($afterHash -eq $beforeHashes[$path]) "Quick validation did not restore $path."
    }
    $status = @(& git -C $repo status --porcelain)
    Assert-True ($status.Count -eq 0) 'Quick validation left tracked or untracked source changes in the fixture repo.'
    $headAfter = (& git -C $repo rev-parse HEAD).Trim()
    Assert-True ($headAfter -eq $headBefore) 'Quick validation created a commit.'
    $tags = @(& git -C $repo tag --list)
    Assert-True ($tags.Count -eq 0) 'Quick validation created a Git tag.'

    $failureWasCaught = $false
    try {
        $env:ACECODE_RELEASE_TEST_FORCE_VERSION = '0.0.0'
        try {
            & $releaseScript `
                -QuickValidation `
                -Version '0.8.7-pre.4' `
                -Repo $repo -BuildDir 'build' `
                -UpdateDir (Join-Path $testRoot 'failed-aupdate') `
                -RemoteBaseUrl '' `
                -SkipBuild
        } finally {
            Remove-Item Env:ACECODE_RELEASE_TEST_FORCE_VERSION -ErrorAction SilentlyContinue
        }
    } catch {
        $failureWasCaught = $_.Exception.Message -like "Built executable reports*"
    }
    Assert-True $failureWasCaught 'Quick validation did not fail on an executable version mismatch.'
    foreach ($path in $sourcePaths) {
        $afterFailureHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        Assert-True ($afterFailureHash -eq $beforeHashes[$path]) "Failed quick validation did not restore $path."
    }

    Write-Host 'Quick validation test passed.'
} finally {
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $tempPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if ($resolvedTestRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTestRoot)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
