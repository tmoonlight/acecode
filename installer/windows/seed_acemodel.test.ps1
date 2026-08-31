$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$seeder = Join-Path $here "seed_acemodel.ps1"
$tempRoot = Join-Path $env:TEMP ("acecode-inno-seed-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    $keyFile = Join-Path $tempRoot "key.txt"
    $configPath = Join-Path $tempRoot "config.json"
    Set-Content -LiteralPath $keyFile -Value "test-ace-key-123" -NoNewline -Encoding ascii
    @'
{
  "provider": "",
  "mcp_servers": {
    "playwright": {
      "args": ["-y", "@playwright/mcp@latest"],
      "command": "npx",
      "disabled": true
    }
  },
  "saved_models": [
    {
      "name": "qwen",
      "provider": "openai",
      "model": "nvidia/nemotron-3-nano-4b",
      "base_url": "http://127.0.0.1:1234/v1",
      "api_key": "local",
      "capabilities": ["tool_use"]
    },
    {
      "name": "moonlight",
      "provider": "openai",
      "model": "moonlight",
      "base_url": "https://old.example/v1",
      "api_key": "old-key"
    },
    {
      "name": "starrylight",
      "provider": "openai",
      "model": "starrylight",
      "base_url": "https://old.example/v1",
      "api_key": "old-key",
      "capabilities": ["tool_use"],
      "capabilities_source": "manual"
    }
  ]
}
'@ | Set-Content -LiteralPath $configPath -Encoding utf8

    & powershell -NoProfile -ExecutionPolicy Bypass -File $seeder -KeyFile $keyFile -ConfigPath $configPath
    if ($LASTEXITCODE -ne 0) { throw "seeder exited $LASTEXITCODE" }

    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $names = @($config.saved_models | ForEach-Object { $_.name })
    if ($names -notcontains "moonlight" -or $names -notcontains "starrylight" -or $names -notcontains "aurora") {
        throw "expected all three ACEModel profiles, got: $($names -join ', ')"
    }
    $moon = $config.saved_models | Where-Object { $_.name -eq "moonlight" } | Select-Object -First 1
    $star = $config.saved_models | Where-Object { $_.name -eq "starrylight" } | Select-Object -First 1
    $aurora = $config.saved_models | Where-Object { $_.name -eq "aurora" } | Select-Object -First 1
    if ($moon.api_key -ne "test-ace-key-123" -or $star.api_key -ne "test-ace-key-123" -or $aurora.api_key -ne "test-ace-key-123") {
        throw "API key was not written into all three profiles"
    }
    if ($moon.base_url -ne "https://ge.bigjuan.xyz/aceapi/v1") {
        throw "existing moonlight profile was not upgraded to ACEModel"
    }
    if ($moon.models_dev_provider_id -ne "acemodel" -or $star.models_dev_provider_id -ne "acemodel" -or $aurora.models_dev_provider_id -ne "acemodel") {
        throw "models_dev_provider_id was not set"
    }
    foreach ($profile in @($moon, $star, $aurora)) {
        if ($profile.context_window -ne 250000) {
            throw "context_window was not set to 250000 for $($profile.name)"
        }
    }
    foreach ($profile in @($moon, $aurora)) {
        if ((@($profile.capabilities) -join ",") -ne "vision,tool_use" -or $profile.capabilities_source -ne "catalog") {
            throw "default vision/tool capabilities were not set for $($profile.name)"
        }
    }
    if ((@($star.capabilities) -join ",") -ne "tool_use" -or $star.capabilities_source -ne "manual") {
        throw "manual ACEModel capabilities were overwritten"
    }
    if ($config.default_model_name -ne "moonlight") {
        throw "default model was not seeded"
    }
    $raw = [System.IO.File]::ReadAllText($configPath)
    if ($raw -notmatch '"capabilities"\s*:\s*\[\s*"tool_use"\s*\]') {
        throw "single-item capabilities array was flattened"
    }
    if ($raw -notmatch '"capabilities"\s*:\s*\[\s*"vision"\s*,\s*"tool_use"\s*\]') {
        throw "ACEModel default capabilities array was not preserved"
    }
    if ($raw -notmatch '"args"\s*:\s*\[\s*"-y"') {
        throw "existing MCP args array was flattened"
    }
    $qwen = $config.saved_models | Where-Object { $_.name -eq "qwen" } | Select-Object -First 1
    if (@($qwen.capabilities).Count -ne 1 -or @($qwen.capabilities)[0] -ne "tool_use") {
        throw "existing qwen capabilities were damaged"
    }
    Write-Host "[pass] ACEModel seeder writes and upgrades all three profiles"
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}
