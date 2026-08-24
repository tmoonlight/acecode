[CmdletBinding()]
param(
    [string]$BaseUrl = 'http://2017studio.imwork.net:82/aupdate/',
    [string]$FeedbackDirectory = 'J:\feedback',
    [string]$ExpectedManifestSha256,
    [string]$ExistingPackage = 'acecode-windows-x64.zip',
    [switch]$RemoveUploadedProbe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$BaseUrl = $BaseUrl.TrimEnd('/') + '/'
$FeedbackDirectory = [System.IO.Path]::GetFullPath($FeedbackDirectory)
$temporaryRoot = Join-Path (
    [System.IO.Path]::GetTempPath()) ('acecode-feedback-verify-' + [Guid]::NewGuid().ToString('N'))
$probeFilename = 'acecode-feedback-verify-' +
    (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.zip'
$probeDirectory = Join-Path $temporaryRoot 'payload'
$probeZip = Join-Path $temporaryRoot $probeFilename
$invalidFilename = 'acecode-feedback-invalid-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.zip'
$invalidPayload = Join-Path $temporaryRoot $invalidFilename
$manifestFile = Join-Path $temporaryRoot 'aceupdate.json'
$uploadedPath = $null

try {
    New-Item -ItemType Directory -Path $probeDirectory -Force | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $probeDirectory 'feedback.json'),
        '{"source":"iis-verification","feedback_text":"probe"}',
        (New-Object System.Text.UTF8Encoding($false)))
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($probeDirectory, $probeZip)
    [System.IO.File]::WriteAllText(
        $invalidPayload,
        'not a zip archive',
        (New-Object System.Text.UTF8Encoding($false)))

    & curl.exe --silent --show-error --fail `
        --output $manifestFile ($BaseUrl + 'aceupdate.json')
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to download aceupdate.json during verification.'
    }
    $manifestHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestFile).Hash.ToLowerInvariant()
    if (-not [string]::IsNullOrWhiteSpace($ExpectedManifestSha256) -and
        $manifestHash -ne $ExpectedManifestSha256.Trim().ToLowerInvariant()) {
        throw "Manifest SHA-256 changed: expected $ExpectedManifestSha256, got $manifestHash"
    }

    $headStatus = & curl.exe --silent --show-error --head --output NUL `
        --write-out '%{http_code}' ($BaseUrl + $ExistingPackage)
    if ($LASTEXITCODE -ne 0 -or $headStatus -ne '200') {
        throw "Existing package HEAD failed with HTTP $headStatus."
    }

    $invalidStatus = & curl.exe --silent --show-error --request POST `
        --header 'Content-Length: 0' --header 'User-Agent: browser' `
        --output NUL --write-out '%{http_code}' $BaseUrl
    if ($LASTEXITCODE -ne 0 -or [int]$invalidStatus -lt 400) {
        throw "Invalid POST was not rejected; HTTP $invalidStatus."
    }

    $childPostStatus = & curl.exe --silent --show-error --request POST `
        --header 'Content-Length: 0' --header 'User-Agent: acecode-feedback' `
        --output NUL --write-out '%{http_code}' ($BaseUrl + 'aceupdate.json')
    if ($LASTEXITCODE -ne 0 -or $childPostStatus -ne '404') {
        throw "Child-path POST was not rejected with HTTP 404; got $childPostStatus."
    }

    $invalidZipStatus = & curl.exe --silent --show-error --request POST `
        --header 'User-Agent: acecode-feedback' `
        --form "file=@$invalidPayload;type=application/zip;filename=$invalidFilename" `
        --form "filename=$invalidFilename" `
        --output NUL --write-out '%{http_code}' $BaseUrl
    if ($LASTEXITCODE -ne 0 -or $invalidZipStatus -ne '415') {
        throw "Non-ZIP payload was not rejected with HTTP 415; got $invalidZipStatus."
    }
    if (Test-Path -LiteralPath (Join-Path $FeedbackDirectory $invalidFilename)) {
        throw 'Rejected non-ZIP payload left a final file in feedback storage.'
    }

    $uploadOutput = @(& curl.exe --silent --show-error `
        --request POST `
        --header 'User-Agent: acecode-feedback' `
        --form "file=@$probeZip;type=application/zip;filename=$probeFilename" `
        --form "filename=$probeFilename" `
        --write-out "`n%{http_code}" `
        $BaseUrl)
    if ($LASTEXITCODE -ne 0 -or $uploadOutput.Count -lt 2) {
        throw 'Compatible multipart upload did not return a response and status line.'
    }
    $uploadStatus = [int]$uploadOutput[-1]
    $uploadBody = ($uploadOutput[0..($uploadOutput.Count - 2)] -join "`n")
    if ($uploadStatus -lt 200 -or $uploadStatus -ge 300) {
        throw "Compatible multipart upload failed with HTTP $uploadStatus`: $uploadBody"
    }
    $uploadJson = $uploadBody | ConvertFrom-Json
    if (-not [bool]$uploadJson.success) {
        throw "Compatible multipart upload returned success=false: $uploadBody"
    }

    $storedFilename = [string]$uploadJson.filename
    if ([System.IO.Path]::GetFileName($storedFilename) -ne $storedFilename) {
        throw 'Server returned an unsafe stored filename.'
    }
    $uploadedPath = Join-Path $FeedbackDirectory $storedFilename
    if (-not (Test-Path -LiteralPath $uploadedPath -PathType Leaf)) {
        throw "Server reported success but the uploaded file is absent: $uploadedPath"
    }
    $probeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $probeZip).Hash.ToLowerInvariant()
    $uploadedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $uploadedPath).Hash.ToLowerInvariant()
    if ($probeHash -ne $uploadedHash) {
        throw 'Stored verification upload does not match the submitted ZIP.'
    }

    [pscustomobject]@{
        ManifestSha256 = $manifestHash
        ExistingPackageHeadStatus = [int]$headStatus
        InvalidPostStatus = [int]$invalidStatus
        ChildPostStatus = [int]$childPostStatus
        InvalidZipStatus = [int]$invalidZipStatus
        UploadStatus = $uploadStatus
        StoredFilename = $storedFilename
        StoredSha256 = $uploadedHash
        RemovedProbe = [bool]$RemoveUploadedProbe
    }

    if ($RemoveUploadedProbe) {
        Remove-Item -LiteralPath $uploadedPath -Force
        $uploadedPath = $null
    }
} finally {
    $tempBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $resolvedTemporaryRoot = [System.IO.Path]::GetFullPath($temporaryRoot)
    if ($resolvedTemporaryRoot.StartsWith(
            $tempBase, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTemporaryRoot)) {
        [System.IO.Directory]::Delete($resolvedTemporaryRoot, $true)
    }
}
