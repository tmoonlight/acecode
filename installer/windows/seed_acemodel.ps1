# Merge ACEModel moonlight / starrylight profiles into the current user's
# ACECode config without flattening existing JSON arrays/objects.
# The API key is read from a temporary file so it never appears on the
# process command line or in Inno Setup's log.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$KeyFile,
    [string]$ConfigPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Web.Extensions

function Read-ApiKey([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "ACEModel key file was not found."
    }
    $raw = [System.IO.File]::ReadAllText($path).Trim()
    if ([string]::IsNullOrWhiteSpace($raw)) {
        throw "ACEModel key file is empty."
    }
    return $raw
}

function Parse-JsonObject([string]$text) {
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = [int]::MaxValue
    $parsed = $serializer.DeserializeObject($text)
    if ($null -eq $parsed) { return New-Object 'System.Collections.Generic.Dictionary[string,object]' }
    if ($parsed -is [System.Collections.IDictionary]) { return $parsed }
    throw "config.json root must be a JSON object"
}

function Escape-JsonString([string]$value) {
    $escaped = $value.Replace('\', '\\').Replace('"', '\"')
    $escaped = $escaped.Replace("`r", '\r').Replace("`n", '\n').Replace("`t", '\t')
    return $escaped
}

function Write-JsonValue($value, [int]$indent) {
    $pad = ' ' * $indent
    $next = ' ' * ($indent + 2)
    if ($null -eq $value) { return 'null' }
    if ($value -is [bool]) { if ($value) { return 'true' } else { return 'false' } }
    if ($value -is [byte] -or $value -is [int16] -or $value -is [uint16] -or
        $value -is [int] -or $value -is [uint32] -or $value -is [int64] -or
        $value -is [uint64] -or $value -is [decimal] -or $value -is [double] -or
        $value -is [single]) {
        return ([string]$value)
    }
    if ($value -is [string]) { return ('"' + (Escape-JsonString $value) + '"') }
    if ($value -is [System.Collections.IDictionary]) {
        $keys = @($value.Keys)
        if ($keys.Count -eq 0) { return '{}' }
        $parts = New-Object System.Collections.Generic.List[string]
        foreach ($key in $keys) {
            $parts.Add($next + '"' + (Escape-JsonString ([string]$key)) + '": ' + (Write-JsonValue $value[$key] ($indent + 2)))
        }
        return ("{`r`n" + ($parts -join ",`r`n") + "`r`n" + $pad + '}')
    }
    if ($value -is [System.Collections.IEnumerable]) {
        $items = @($value)
        if ($items.Count -eq 0) { return '[]' }
        $parts = New-Object System.Collections.Generic.List[string]
        foreach ($item in $items) {
            $parts.Add($next + (Write-JsonValue $item ($indent + 2)))
        }
        return ("[`r`n" + ($parts -join ",`r`n") + "`r`n" + $pad + ']')
    }
    return ('"' + (Escape-JsonString ([string]$value)) + '"')
}

function To-Json([object]$value) {
    return ((Write-JsonValue $value 0) + "`r`n")
}

function Config-HasKey($config, [string]$name) {
    if ($null -eq $config) { return $false }
    foreach ($key in @($config.Keys)) {
        if ([string]$key -eq $name) { return $true }
    }
    return $false
}

function Copy-ToList($raw) {
    $list = @()
    if ($null -eq $raw -or $raw -is [string]) { return $list }
    foreach ($item in @($raw)) { $list += ,$item }
    return $list
}

function Get-SavedModels($config) {
    if (-not (Config-HasKey $config "saved_models") -or $null -eq $config["saved_models"]) {
        return @()
    }
    return (Copy-ToList $config["saved_models"])
}

function New-AceModelProfile([string]$name, [string]$key) {
    $profile = New-Object 'System.Collections.Generic.Dictionary[string,object]'
    $profile["name"] = $name
    $profile["provider"] = "openai"
    $profile["model"] = $name
    $profile["base_url"] = "https://ge.bigjuan.xyz/aceapi/v1"
    $profile["api_key"] = $key
    $profile["models_dev_provider_id"] = "acemodel"
    $profile["endpoint_mode"] = "base_url"
    $profile["capabilities"] = @("tool_use")
    $profile["capabilities_source"] = "catalog"
    return $profile
}

function Upsert-AceModel($models, [string]$name, [string]$key) {
    $next = @(Copy-ToList $models)
    for ($index = 0; $index -lt $next.Count; $index++) {
        $current = $next[$index]
        if (-not ($current -is [System.Collections.IDictionary])) { continue }
        $currentName = [string]$current["name"]
        $currentModel = [string]$current["model"]
        if ($currentName -eq $name -or $currentModel -eq $name) {
            $current["name"] = $name
            $current["provider"] = "openai"
            $current["model"] = $name
            $current["base_url"] = "https://ge.bigjuan.xyz/aceapi/v1"
            $current["api_key"] = $key
            $current["models_dev_provider_id"] = "acemodel"
            if (-not (Config-HasKey $current "endpoint_mode") -or [string]::IsNullOrWhiteSpace([string]$current["endpoint_mode"])) {
                $current["endpoint_mode"] = "base_url"
            }
            $next[$index] = $current
            return @($next)
        }
    }
    return @($next + ,(New-AceModelProfile -name $name -key $key))
}

$key = Read-ApiKey -path $KeyFile
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $env:USERPROFILE ".acecode\config.json"
}

$configDir = Split-Path -Parent $ConfigPath
if (-not (Test-Path -LiteralPath $configDir)) {
    New-Item -ItemType Directory -Path $configDir | Out-Null
}

$config = New-Object 'System.Collections.Generic.Dictionary[string,object]'
if (Test-Path -LiteralPath $ConfigPath) {
    $text = [System.IO.File]::ReadAllText($ConfigPath)
    if (-not [string]::IsNullOrWhiteSpace($text)) {
        $config = Parse-JsonObject $text
    }
}

$models = Get-SavedModels $config
$models = Upsert-AceModel -models $models -name "moonlight" -key $key
$models = Upsert-AceModel -models $models -name "starrylight" -key $key
$config["saved_models"] = $models

if (-not (Config-HasKey $config "default_model_name") -or [string]::IsNullOrWhiteSpace([string]$config["default_model_name"])) {
    $config["default_model_name"] = "moonlight"
}

$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($ConfigPath, (To-Json $config), $utf8)
