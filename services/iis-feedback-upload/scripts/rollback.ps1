[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [string]$BackupDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
}

function Copy-FileAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $temporary = $Destination + '.rollback-' + [Guid]::NewGuid().ToString('N') + '.tmp'
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

$BackupDirectory = Get-NormalizedPath $BackupDirectory
$manifestPath = Join-Path $BackupDirectory 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Rollback manifest does not exist: $manifestPath"
}
$manifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
if ($manifest.schema_version -ne 1) {
    throw "Unsupported rollback manifest version: $($manifest.schema_version)"
}

$siteRoot = Get-NormalizedPath ([string]$manifest.site_root)
$updateDirectory = Get-NormalizedPath ([string]$manifest.update_directory)
$rootWebConfig = Get-NormalizedPath ([string]$manifest.root_web_config)
$updateWebConfig = Get-NormalizedPath ([string]$manifest.update_web_config)
$handlerAssembly = Get-NormalizedPath ([string]$manifest.handler_assembly)

if (-not $rootWebConfig.Equals(
        (Get-NormalizedPath (Join-Path $siteRoot 'web.config')),
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Rollback manifest root web.config target is inconsistent with site_root.'
}
if (-not $updateWebConfig.Equals(
        (Get-NormalizedPath (Join-Path $updateDirectory 'web.config')),
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Rollback manifest update web.config target is inconsistent with update_directory.'
}
if (-not $handlerAssembly.Equals(
        (Get-NormalizedPath (Join-Path $siteRoot 'bin\Acecode.FeedbackUpload.dll')),
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Rollback manifest handler target is inconsistent with site_root.'
}

$rootBackup = Join-Path $BackupDirectory 'root.web.config'
$updateBackup = Join-Path $BackupDirectory 'aupdate.web.config'
if (-not (Test-Path -LiteralPath $rootBackup -PathType Leaf) -or
    -not (Test-Path -LiteralPath $updateBackup -PathType Leaf)) {
    throw 'Rollback configuration backups are incomplete.'
}

if (-not $PSCmdlet.ShouldProcess(
        $siteRoot,
        "Restore IIS feedback-handler state from $BackupDirectory")) {
    return
}

Copy-FileAtomic -Source $rootBackup -Destination $rootWebConfig
Copy-FileAtomic -Source $updateBackup -Destination $updateWebConfig

if ([bool]$manifest.handler_assembly_existed) {
    $assemblyBackup = Join-Path $BackupDirectory 'Acecode.FeedbackUpload.dll'
    if (-not (Test-Path -LiteralPath $assemblyBackup -PathType Leaf)) {
        throw 'Rollback manifest expects a prior handler assembly, but its backup is missing.'
    }
    Copy-FileAtomic -Source $assemblyBackup -Destination $handlerAssembly
} elseif (Test-Path -LiteralPath $handlerAssembly -PathType Leaf) {
    Remove-Item -LiteralPath $handlerAssembly -Force
}

[pscustomobject]@{
    RestoredFrom = $BackupDirectory
    SiteRoot = $siteRoot
    FeedbackDirectoryUntouched = [string]$manifest.feedback_directory
}
