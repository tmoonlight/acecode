[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$SiteRoot = 'J:\jenkins_green',
    [string]$UpdateDirectory = 'J:\jenkins_green\aupdate',
    [string]$FeedbackDirectory = 'J:\feedback',
    [string]$EndpointPath = '/aupdate/',
    [long]$MaxFileBytes = 67108864,
    [long]$MinimumFreeBytes = 1073741824,
    [string]$BackupRoot = 'J:\acecode-feedback-deploy-backups'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
}

function Test-PathWithinDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Directory
    )
    $candidatePath = Get-NormalizedPath $Candidate
    $directoryPath = Get-NormalizedPath $Directory
    if ($candidatePath.Equals($directoryPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    return $candidatePath.StartsWith(
        $directoryPath + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-DirectChild {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlNode]$Parent,
        [Parameter(Mandatory = $true)][string]$Name
    )
    foreach ($child in @($Parent.ChildNodes)) {
        if ($child.NodeType -eq [System.Xml.XmlNodeType]::Element -and
            $child.LocalName -eq $Name) {
            return $child
        }
    }
    return $null
}

function Get-OrCreateDirectChild {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlDocument]$Document,
        [Parameter(Mandatory = $true)][System.Xml.XmlNode]$Parent,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $child = Get-DirectChild -Parent $Parent -Name $Name
    if ($child) {
        return $child
    }
    $child = $Document.CreateElement($Name)
    [void]$Parent.AppendChild($child)
    return $child
}

function Set-AppSetting {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlDocument]$Document,
        [Parameter(Mandatory = $true)][System.Xml.XmlNode]$AppSettings,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string]$Value
    )
    $setting = $null
    foreach ($child in @($AppSettings.ChildNodes)) {
        if ($child.NodeType -eq [System.Xml.XmlNodeType]::Element -and
            $child.LocalName -eq 'add' -and
            $child.GetAttribute('key') -eq $Key) {
            if (-not $setting) {
                $setting = $child
            } else {
                [void]$AppSettings.RemoveChild($child)
            }
        }
    }
    if (-not $setting) {
        $setting = $Document.CreateElement('add')
        [void]$AppSettings.AppendChild($setting)
    }
    $setting.SetAttribute('key', $Key)
    $setting.SetAttribute('value', $Value)
}

function Remove-HandlerNodes {
    param([Parameter(Mandatory = $true)][System.Xml.XmlNode]$Handlers)
    foreach ($child in @($Handlers.ChildNodes)) {
        if ($child.NodeType -ne [System.Xml.XmlNodeType]::Element) {
            continue
        }
        $name = $child.GetAttribute('name')
        if ($name -eq 'AcecodeFeedbackUpload' -or $name -eq 'StaticFile') {
            [void]$Handlers.RemoveChild($child)
        }
    }
}

function Convert-XmlToUtf8Text {
    param([Parameter(Mandatory = $true)][System.Xml.XmlDocument]$Document)
    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Encoding = New-Object System.Text.UTF8Encoding($false)
    $settings.Indent = $true
    $settings.IndentChars = '  '
    $settings.NewLineChars = "`r`n"
    $settings.NewLineHandling = [System.Xml.NewLineHandling]::Replace
    $settings.OmitXmlDeclaration = $false
    $stream = New-Object System.IO.MemoryStream
    try {
        $writer = [System.Xml.XmlWriter]::Create($stream, $settings)
        try {
            $Document.Save($writer)
        } finally {
            $writer.Dispose()
        }
        return [System.Text.Encoding]::UTF8.GetString($stream.ToArray())
    } finally {
        $stream.Dispose()
    }
}

function Read-XmlDocument {
    param([Parameter(Mandatory = $true)][string]$Path)
    $document = New-Object System.Xml.XmlDocument
    $document.PreserveWhitespace = $false
    $document.LoadXml([System.IO.File]::ReadAllText($Path))
    if (-not $document.DocumentElement -or $document.DocumentElement.Name -ne 'configuration') {
        throw "Expected a <configuration> root in $Path"
    }
    return $document
}

function Write-Utf8TextAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )
    $temporary = $Path + '.uploading-' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $replaceBackup = $Path + '.replace-backup-' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [System.IO.File]::WriteAllText(
            $temporary, $Text, (New-Object System.Text.UTF8Encoding($false)))
        if (Test-Path -LiteralPath $Path) {
            [System.IO.File]::Replace($temporary, $Path, $replaceBackup)
        } else {
            [System.IO.File]::Move($temporary, $Path)
        }
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
        if (Test-Path -LiteralPath $replaceBackup) {
            Remove-Item -LiteralPath $replaceBackup -Force
        }
    }
}

function Copy-FileAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $temporary = $Destination + '.uploading-' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $replaceBackup = $Destination + '.replace-backup-' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [System.IO.File]::Copy($Source, $temporary, $true)
        if (Test-Path -LiteralPath $Destination) {
            [System.IO.File]::Replace($temporary, $Destination, $replaceBackup)
        } else {
            [System.IO.File]::Move($temporary, $Destination)
        }
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
        if (Test-Path -LiteralPath $replaceBackup) {
            Remove-Item -LiteralPath $replaceBackup -Force
        }
    }
}

function Set-RestrictedDirectoryAcl {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$AllowIisModify
    )
    $icacls = Join-Path $env:WINDIR 'System32\icacls.exe'
    $currentSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $grants = @(
        "*$($currentSid):(OI)(CI)(F)",
        '*S-1-5-32-544:(OI)(CI)(F)',
        '*S-1-5-18:(OI)(CI)(F)'
    )
    if ($AllowIisModify) {
        $grants += '*S-1-5-32-568:(OI)(CI)(M)'
    }

    & $icacls $Path '/grant:r' @grants | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to set explicit access rules on $Path."
    }
    & $icacls $Path '/inheritance:r' | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to disable inherited access rules on $Path."
    }
}

$SiteRoot = Get-NormalizedPath $SiteRoot
$UpdateDirectory = Get-NormalizedPath $UpdateDirectory
$FeedbackDirectory = Get-NormalizedPath $FeedbackDirectory
$BackupRoot = Get-NormalizedPath $BackupRoot

if (-not (Test-Path -LiteralPath $SiteRoot -PathType Container)) {
    throw "IIS site root does not exist: $SiteRoot"
}
if (-not (Test-Path -LiteralPath $UpdateDirectory -PathType Container)) {
    throw "IIS update directory does not exist: $UpdateDirectory"
}
if (-not (Test-PathWithinDirectory -Candidate $UpdateDirectory -Directory $SiteRoot)) {
    throw 'The update directory must be inside the IIS site root.'
}
if (Test-PathWithinDirectory -Candidate $FeedbackDirectory -Directory $SiteRoot) {
    throw 'The feedback directory must be outside the public IIS site root.'
}
if (Test-PathWithinDirectory -Candidate $BackupRoot -Directory $SiteRoot) {
    throw 'The deployment backup directory must be outside the public IIS site root.'
}
if ($MaxFileBytes -le 0 -or $MinimumFreeBytes -lt 0) {
    throw 'Upload and free-space limits must be positive values.'
}
if ($MaxFileBytes -gt ([uint32]::MaxValue - 1048576)) {
    throw 'MaxFileBytes is too large for the IIS request-filtering limit.'
}

$normalizedEndpoint = '/' + $EndpointPath.Trim().Replace('\', '/').Trim('/') + '/'
$locationPath = $normalizedEndpoint.Trim('/')
if ([string]::IsNullOrWhiteSpace($locationPath) -or $locationPath.Contains('..')) {
    throw 'EndpointPath must name a non-root path without traversal.'
}
$expectedUpdateDirectory = Get-NormalizedPath (
    Join-Path $SiteRoot $locationPath.Replace('/', [System.IO.Path]::DirectorySeparatorChar))
if (-not $expectedUpdateDirectory.Equals(
        $UpdateDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "UpdateDirectory does not match EndpointPath under SiteRoot: $expectedUpdateDirectory"
}

$rootWebConfig = Join-Path $SiteRoot 'web.config'
$updateWebConfig = Join-Path $UpdateDirectory 'web.config'
if (-not (Test-Path -LiteralPath $rootWebConfig -PathType Leaf)) {
    throw "Root web.config does not exist: $rootWebConfig"
}
if (-not (Test-Path -LiteralPath $updateWebConfig -PathType Leaf)) {
    throw "Update web.config does not exist: $updateWebConfig"
}

$componentRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot 'build.ps1'
$buildOutput = Join-Path $componentRoot 'out'
$buildResult = & $buildScript -OutputDirectory $buildOutput
$handlerSourceAssembly = Join-Path $buildOutput 'Acecode.FeedbackUpload.dll'
if (-not (Test-Path -LiteralPath $handlerSourceAssembly -PathType Leaf)) {
    throw 'The handler build did not produce Acecode.FeedbackUpload.dll.'
}

$rootDocument = Read-XmlDocument $rootWebConfig
$configuration = $rootDocument.DocumentElement
$appSettings = Get-OrCreateDirectChild -Document $rootDocument -Parent $configuration -Name 'appSettings'
Set-AppSetting -Document $rootDocument -AppSettings $appSettings `
    -Key 'AcecodeFeedback.StoragePath' -Value $FeedbackDirectory
Set-AppSetting -Document $rootDocument -AppSettings $appSettings `
    -Key 'AcecodeFeedback.EndpointPath' -Value $normalizedEndpoint
Set-AppSetting -Document $rootDocument -AppSettings $appSettings `
    -Key 'AcecodeFeedback.MaxFileBytes' -Value $MaxFileBytes.ToString([System.Globalization.CultureInfo]::InvariantCulture)
Set-AppSetting -Document $rootDocument -AppSettings $appSettings `
    -Key 'AcecodeFeedback.MinimumFreeBytes' -Value $MinimumFreeBytes.ToString([System.Globalization.CultureInfo]::InvariantCulture)

$location = $null
foreach ($child in @($configuration.ChildNodes)) {
    if ($child.NodeType -eq [System.Xml.XmlNodeType]::Element -and
        $child.LocalName -eq 'location' -and
        $child.GetAttribute('path') -eq $locationPath) {
        $location = $child
        break
    }
}
if (-not $location) {
    $location = $rootDocument.CreateElement('location')
    $location.SetAttribute('path', $locationPath)
    [void]$configuration.AppendChild($location)
}
$location.SetAttribute('inheritInChildApplications', 'false')
$systemWeb = Get-OrCreateDirectChild -Document $rootDocument -Parent $location -Name 'system.web'
$httpRuntime = Get-OrCreateDirectChild -Document $rootDocument -Parent $systemWeb -Name 'httpRuntime'
$requestLimitBytes = $MaxFileBytes + 1048576
$requestLimitKilobytes = [long][Math]::Ceiling($requestLimitBytes / 1024.0)
$httpRuntime.SetAttribute(
    'maxRequestLength', $requestLimitKilobytes.ToString([System.Globalization.CultureInfo]::InvariantCulture))
$httpRuntime.SetAttribute('executionTimeout', '120')

$updateDocument = Read-XmlDocument $updateWebConfig
$updateConfiguration = $updateDocument.DocumentElement
$systemWebServer = Get-OrCreateDirectChild `
    -Document $updateDocument -Parent $updateConfiguration -Name 'system.webServer'
$security = Get-OrCreateDirectChild `
    -Document $updateDocument -Parent $systemWebServer -Name 'security'
$requestFiltering = Get-OrCreateDirectChild `
    -Document $updateDocument -Parent $security -Name 'requestFiltering'
$requestLimits = Get-OrCreateDirectChild `
    -Document $updateDocument -Parent $requestFiltering -Name 'requestLimits'
$requestLimits.SetAttribute(
    'maxAllowedContentLength', $requestLimitBytes.ToString([System.Globalization.CultureInfo]::InvariantCulture))

$handlers = Get-OrCreateDirectChild `
    -Document $updateDocument -Parent $systemWebServer -Name 'handlers'
Remove-HandlerNodes $handlers
$hasClear = $false
foreach ($child in @($handlers.ChildNodes)) {
    if ($child.NodeType -eq [System.Xml.XmlNodeType]::Element -and
        $child.LocalName -eq 'clear') {
        $hasClear = $true
        break
    }
}
if (-not $hasClear) {
    $removeStatic = $updateDocument.CreateElement('remove')
    $removeStatic.SetAttribute('name', 'StaticFile')
    [void]$handlers.AppendChild($removeStatic)
}
$feedbackHandler = $updateDocument.CreateElement('add')
$feedbackHandler.SetAttribute('name', 'AcecodeFeedbackUpload')
$feedbackHandler.SetAttribute('path', '*')
$feedbackHandler.SetAttribute('verb', 'POST')
$feedbackHandler.SetAttribute(
    'type', 'Acecode.FeedbackUpload.FeedbackUploadHandler, Acecode.FeedbackUpload')
$feedbackHandler.SetAttribute('resourceType', 'Unspecified')
$feedbackHandler.SetAttribute('preCondition', 'integratedMode,runtimeVersionv4.0')
[void]$handlers.AppendChild($feedbackHandler)

$staticHandler = $updateDocument.CreateElement('add')
$staticHandler.SetAttribute('name', 'StaticFile')
$staticHandler.SetAttribute('path', '*')
$staticHandler.SetAttribute('verb', 'GET,HEAD')
$staticHandler.SetAttribute(
    'modules', 'StaticFileModule,DefaultDocumentModule,DirectoryListingModule')
$staticHandler.SetAttribute('resourceType', 'Either')
$staticHandler.SetAttribute('requireAccess', 'Read')
[void]$handlers.AppendChild($staticHandler)

$rootCandidateText = Convert-XmlToUtf8Text $rootDocument
$updateCandidateText = Convert-XmlToUtf8Text $updateDocument
$validationDocument = New-Object System.Xml.XmlDocument
$validationDocument.LoadXml($rootCandidateText)
$validationDocument.LoadXml($updateCandidateText)

$handlerTargetDirectory = Join-Path $SiteRoot 'bin'
$handlerTargetAssembly = Join-Path $handlerTargetDirectory 'Acecode.FeedbackUpload.dll'
$backupId = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmssfff') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 8)
$backupDirectory = Join-Path $BackupRoot $backupId

$summary = [ordered]@{
    SiteRoot = $SiteRoot
    UpdateDirectory = $UpdateDirectory
    FeedbackDirectory = $FeedbackDirectory
    EndpointPath = $normalizedEndpoint
    HandlerAssembly = $handlerTargetAssembly
    HandlerSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $handlerSourceAssembly).Hash.ToLowerInvariant()
    BackupDirectory = $backupDirectory
    MaxFileBytes = $MaxFileBytes
    MinimumFreeBytes = $MinimumFreeBytes
}

if (-not $PSCmdlet.ShouldProcess(
        $UpdateDirectory,
        "Install the ACECode feedback POST handler and configure storage at $FeedbackDirectory")) {
    [pscustomobject]$summary
    return
}

$backupCreated = $false
$assemblyExisted = Test-Path -LiteralPath $handlerTargetAssembly -PathType Leaf
try {
    New-Item -ItemType Directory -Force -Path $FeedbackDirectory | Out-Null
    New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null
    New-Item -ItemType Directory -Path $backupDirectory | Out-Null
    $backupCreated = $true

    [System.IO.File]::Copy($rootWebConfig, (Join-Path $backupDirectory 'root.web.config'), $false)
    [System.IO.File]::Copy($updateWebConfig, (Join-Path $backupDirectory 'aupdate.web.config'), $false)
    if ($assemblyExisted) {
        [System.IO.File]::Copy(
            $handlerTargetAssembly,
            (Join-Path $backupDirectory 'Acecode.FeedbackUpload.dll'),
            $false)
    }

    $manifest = [ordered]@{
        schema_version = 1
        created_at = (Get-Date).ToUniversalTime().ToString('o')
        site_root = $SiteRoot
        update_directory = $UpdateDirectory
        feedback_directory = $FeedbackDirectory
        root_web_config = $rootWebConfig
        update_web_config = $updateWebConfig
        handler_assembly = $handlerTargetAssembly
        handler_assembly_existed = $assemblyExisted
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $backupDirectory 'manifest.json'),
        ($manifest | ConvertTo-Json -Depth 4),
        (New-Object System.Text.UTF8Encoding($false)))

    Set-RestrictedDirectoryAcl -Path $BackupRoot
    Set-RestrictedDirectoryAcl -Path $FeedbackDirectory -AllowIisModify

    New-Item -ItemType Directory -Force -Path $handlerTargetDirectory | Out-Null
    Copy-FileAtomic -Source $handlerSourceAssembly -Destination $handlerTargetAssembly
    Write-Utf8TextAtomic -Path $rootWebConfig -Text $rootCandidateText
    Write-Utf8TextAtomic -Path $updateWebConfig -Text $updateCandidateText
} catch {
    $deploymentError = $_
    if ($backupCreated) {
        try {
            Copy-FileAtomic `
                -Source (Join-Path $backupDirectory 'root.web.config') `
                -Destination $rootWebConfig
            Copy-FileAtomic `
                -Source (Join-Path $backupDirectory 'aupdate.web.config') `
                -Destination $updateWebConfig
            if ($assemblyExisted) {
                Copy-FileAtomic `
                    -Source (Join-Path $backupDirectory 'Acecode.FeedbackUpload.dll') `
                    -Destination $handlerTargetAssembly
            } elseif (Test-Path -LiteralPath $handlerTargetAssembly) {
                Remove-Item -LiteralPath $handlerTargetAssembly -Force
            }
        } catch {
            Write-Warning (
                "Automatic rollback also failed: " + $_.Exception.Message +
                ". Use backup: $backupDirectory")
        }
    }
    throw $deploymentError
}

[pscustomobject]$summary
