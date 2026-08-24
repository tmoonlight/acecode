[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$componentRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $componentRoot 'out'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

$compilerCandidates = @(
    (Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'),
    (Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe')
)
$compiler = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $compiler) {
    throw 'The .NET Framework 4 C# compiler was not found.'
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$handlerSource = Join-Path $componentRoot 'src\AcecodeFeedbackUploadHandler.cs'
$testSource = Join-Path $componentRoot 'tests\AcecodeFeedbackUploadHandlerTests.cs'
$handlerAssembly = Join-Path $OutputDirectory 'Acecode.FeedbackUpload.dll'
$testExecutable = Join-Path $OutputDirectory 'Acecode.FeedbackUpload.Tests.exe'

$frameworkDirectory = Split-Path -Parent $compiler
$references = @(
    (Join-Path $frameworkDirectory 'System.dll'),
    (Join-Path $frameworkDirectory 'System.Core.dll'),
    (Join-Path $frameworkDirectory 'System.Configuration.dll'),
    (Join-Path $frameworkDirectory 'System.Web.dll')
)
foreach ($reference in $references) {
    if (-not (Test-Path -LiteralPath $reference)) {
        throw "Required .NET Framework assembly was not found: $reference"
    }
}

$libraryArguments = @(
    '/nologo',
    '/target:library',
    '/optimize+',
    '/warnaserror+',
    "/out:$handlerAssembly"
)
$libraryArguments += $references | ForEach-Object { "/reference:$_" }
$libraryArguments += $handlerSource
& $compiler @libraryArguments
if ($LASTEXITCODE -ne 0) {
    throw "Handler compilation failed with exit code $LASTEXITCODE."
}

if (-not $SkipTests) {
    $testArguments = @(
        '/nologo',
        '/target:exe',
        '/optimize+',
        '/warnaserror+',
        "/out:$testExecutable",
        "/reference:$handlerAssembly",
        $testSource
    )
    & $compiler @testArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Policy-test compilation failed with exit code $LASTEXITCODE."
    }

    & $testExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "Policy tests failed with exit code $LASTEXITCODE."
    }
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $handlerAssembly).Hash.ToLowerInvariant()
[pscustomobject]@{
    HandlerAssembly = $handlerAssembly
    Sha256 = $hash
    TestsRun = -not $SkipTests
}
