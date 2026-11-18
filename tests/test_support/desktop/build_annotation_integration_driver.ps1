param(
    [Parameter(Mandatory = $true)][string]$DependencyInclude,
    [string]$OutputDirectory = (Join-Path $env:TEMP 'acecode-annotation-integration'),
    [string]$VcVarsPath = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$DependencyInclude = (Resolve-Path -LiteralPath $DependencyInclude).Path
if (-not (Test-Path -LiteralPath (Join-Path $DependencyInclude 'nlohmann/json.hpp'))) {
    throw 'DependencyInclude must contain nlohmann/json.hpp'
}
if (-not $env:INCLUDE) {
    if (-not (Test-Path -LiteralPath $VcVarsPath)) { throw 'Run in an MSVC developer shell or supply VcVarsPath' }
    $environment = & cmd /c "`"$VcVarsPath`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize MSVC environment' }
    foreach ($entry in $environment) {
        if ($entry -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$arguments = @('/nologo', '/std:c++17', '/EHsc', '/utf-8', '/MT', '/O2', '/Gy')
foreach ($group in @('apps', 'base', 'domain', 'engine', 'host', 'adapters')) {
    $arguments += '/I' + (Join-Path $repository "src/$group")
}
$arguments += '/I' + $DependencyInclude
$arguments += @(
    (Join-Path $PSScriptRoot 'page_annotation_integration_driver.cpp'),
    (Join-Path $repository 'src/engine/agent/worker/agent_task_queue.cpp'),
    (Join-Path $repository 'src/base/utils/append_file.cpp'),
    ('/Fo' + $OutputDirectory + '\'),
    ('/Fe' + (Join-Path $OutputDirectory 'annotation_integration_driver.exe')),
    '/link', '/OPT:REF'
)
& cl @arguments
if ($LASTEXITCODE -ne 0) { throw "Native driver compilation failed: $LASTEXITCODE" }
Write-Output (Join-Path $OutputDirectory 'annotation_integration_driver.exe')
