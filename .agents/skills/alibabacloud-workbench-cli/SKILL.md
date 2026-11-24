---
name: alibabacloud-workbench-cli
description: Agent-native CLI for managing ECS instances without public IPs, primarily for single-instance operations. It supports millisecond-level remote command execution and large file transfers up to 1GB. It offers seven authentication modes: AK, StsToken, RamRoleArn, CredentialsCmd, CredentialsURI, EcsRamRole, and AliyunCLI. Use it to run commands, deploy code, view logs, check processes, transfer files, or query and filter ECS instance lists. Requires the workbench CLI binary installed (Linux/macOS amd64/arm64, Windows amd64) and network access to *.aliyuncs.com and Workbench backend WebSocket endpoints.
version: 1.1.1
---

# Workbench CLI Expert

Help users operate Alibaba Cloud ECS instances — especially those **without public IP addresses** — using the `workbench` command-line tool. Core capabilities for Agent workflows: millisecond-level remote command execution (`exec`) and file transfer up to 1GB (`upload`/`download`). This skill covers: install → configure credentials → exec/transfer → manage sessions → troubleshoot errors.

> **Running inside the `alibabacloud-workbench` plugin?** Then a SessionStart hook already checked (and, if needed, installed) the CLI, and injected the binary path plus credential state into the conversation. Two helpers are available — always call them with `bash`:
> `bash "${QODER_PLUGIN_ROOT}/scripts/install-cli.sh"` (install/upgrade, user scope, no sudo) and
> `bash "${QODER_PLUGIN_ROOT}/scripts/wb-config.sh" check|set|verify|show` (credentials).
> The `/alibabacloud-workbench:setup` command wraps both; `/alibabacloud-workbench:doctor` diagnoses.
> Skip to step 3 whenever the injected status says the CLI is present and credentials are `ok`.

## Instructions

### 1. Install the Workbench CLI

**Pre-check:**

```bash
workbench version     # Should print version, commit, build date
```

If `workbench` is not on `PATH`, it may still be installed in a user-scope directory. Check, in order:
`~/.workbench/qoder/bin-path` (written by the plugin), `/usr/local/bin/workbench`, `~/.local/bin/workbench`.
When the binary is not on `PATH`, invoke it by absolute path for every command instead of editing shell profiles.

**Plugin-managed install (preferred inside Qoder):**

```bash
bash "${QODER_PLUGIN_ROOT}/scripts/install-cli.sh"              # latest, no sudo
bash "${QODER_PLUGIN_ROOT}/scripts/install-cli.sh" --version 0.2.0 --dir ~/.local/bin
```

It reuses the official installer (checksum-verified) and picks the first writable directory among
`/usr/local/bin`, `~/.local/bin`, and the plugin data dir. Set `WORKBENCH_QODER_AUTO_INSTALL=0` to disable
the automatic background install entirely; the install log is `${QODER_PLUGIN_DATA}/bootstrap.log`.

**Linux / macOS (manual):**

```bash
curl -fsSL https://workbench-cli.oss-cn-hangzhou.aliyuncs.com/install.sh | bash
```

**Windows (PowerShell):**

```powershell
irm https://workbench-cli.oss-cn-hangzhou.aliyuncs.com/install.ps1 | iex
```

Installs to `C:\Program Files\workbench\`. Note: on Desktop/RDP sessions, logoff and re-login is required for PATH to take effect.

**Upgrade:**

```bash
workbench upgrade                        # Upgrade to latest
workbench upgrade --version 0.2.0        # Upgrade to specific version
```

### 2. Configure credentials

Credentials are stored in `~/.workbench/config.json` with `0600` permissions. **Never run bare `workbench config`** in an Agent session — it is interactive and will hang waiting on a TTY.

**Plugin-managed (preferred): `wb-config.sh`** — merges into existing profiles, enforces `0600`, and never echoes the secret. Pass the AccessKeySecret on **stdin** so it never lands in the process list:

```bash
# state of install + credentials (text or --json)
bash "${QODER_PLUGIN_ROOT}/scripts/wb-config.sh" check --json

# AK mode
printf '%s' '<AccessKeySecret>' | bash "${QODER_PLUGIN_ROOT}/scripts/wb-config.sh" \
  set --mode AK --access-key-id '<AccessKeyId>' --access-key-secret-stdin

# RamRoleArn mode (production; STS tokens auto-refresh)
printf '%s' '<AccessKeySecret>' | bash "${QODER_PLUGIN_ROOT}/scripts/wb-config.sh" \
  set --mode RamRoleArn --access-key-id '<AccessKeyId>' --access-key-secret-stdin \
      --ram-role-arn 'acs:ram::<account-id>:role/<role>' --profile prod --no-activate

# keyless modes
bash "${QODER_PLUGIN_ROOT}/scripts/wb-config.sh" set --mode CredentialsURI --credentials-uri '<url>'
bash "${QODER_PLUGIN_ROOT}/scripts/wb-config.sh" set --mode CredentialsCmd --credentials-cmd '<command>'
bash "${QODER_PLUGIN_ROOT}/scripts/wb-config.sh" set --mode AliyunCLI --aliyun-profile '<profile>'   # reuse local aliyun CLI; omit --aliyun-profile to follow its current

# live check: one real API call
bash "${QODER_PLUGIN_ROOT}/scripts/wb-config.sh" verify --region cn-hangzhou
```

The helper also accepts `WORKBENCH_ACCESS_KEY_SECRET` / `WORKBENCH_STS_TOKEN` from the environment, which is the
right choice when the user does not want to paste the secret into the conversation.

**Fallback (no plugin helper available): write the file directly.**

```bash
# Create config directory
mkdir -p ~/.workbench

# Write config file (example: AK mode)
cat > ~/.workbench/config.json << 'EOF'
{
  "current": "default",
  "profiles": {
    "default": {
      "mode": "AK",
      "access_key_id": "<AccessKeyID>",
      "access_key_secret": "<AccessKeySecret>"
    }
  }
}
EOF

# Set secure permissions
chmod 600 ~/.workbench/config.json
```

**Config file schema by mode:**

AK mode:
```json
{
  "current": "default",
  "profiles": {
    "default": {
      "mode": "AK",
      "access_key_id": "LTAI...",
      "access_key_secret": "..."
    }
  }
}
```

RamRoleArn mode (auto-refreshes STS tokens):
```json
{
  "current": "default",
  "profiles": {
    "default": {
      "mode": "RamRoleArn",
      "access_key_id": "LTAI...",
      "access_key_secret": "...",
      "ram_role_arn": "acs:ram::123456789:role/WorkbenchRole",
      "role_session_name": "workbench-session"
    }
  }
}
```

CredentialsURI mode (HTTP endpoint returns credentials):
```json
{
  "current": "default",
  "profiles": {
    "default": {
      "mode": "CredentialsURI",
      "credentials_uri": "http://localhost:8080/credentials"
    }
  }
}
```

EcsRamRole mode (auto-fetch from IMDS on an ECS instance, no AccessKey):
```json
{
  "current": "default",
  "profiles": {
    "default": {
      "mode": "EcsRamRole"
    }
  }
}
```
> `role_name` is optional (auto-detected from IMDS when omitted). `disable_imds_v1` defaults to `true` (IMDSv2 hardened); set to `false` to allow IMDSv1 fallback on older instances. Only works when the CLI runs on an Alibaba Cloud ECS/ECI instance bound to a RAM role.

AliyunCLI mode (reuse credentials from the local Alibaba Cloud CLI, no AccessKey stored in workbench):
```json
{
  "current": "default",
  "profiles": {
    "default": {
      "mode": "AliyunCLI",
      "aliyun_profile": "myprofile"
    }
  }
}
```
> `aliyun_profile` is optional: omit it to follow the aliyun CLI's current profile (which also honours the `ALIBABA_CLOUD_PROFILE` environment variable). Requires the Alibaba Cloud CLI (`aliyun`) installed and configured locally (`~/.aliyun/config.json`).

| Mode | When to use |
| --- | --- |
| **AK** (default) | Development, long-lived credentials |
| **StsToken** | Temporary security credentials (AccessKey + STS Token) |
| **RamRoleArn** | Production, cross-account, least-privilege via STS role assumption (auto-refreshes tokens) |
| **CredentialsCmd** | Zero-trust / Vault integration — external command outputs credential JSON |
| **CredentialsURI** | Metadata service / sidecar — HTTP endpoint returns credential JSON |
| **EcsRamRole** | CLI runs on an ECS instance bound to a RAM role — auto-fetches STS credentials from IMDS, no AccessKey needed |
| **AliyunCLI** | Reuse credentials from the local Alibaba Cloud CLI (`~/.aliyun/config.json`) — no AccessKey stored in workbench |

**Profile management (non-interactive):**

```bash
workbench config list                     # List all profiles (* marks active)
workbench config switch --profile prod    # Switch active profile
workbench config get                      # Show current profile details (JSON)
workbench config get --profile prod       # Show specific profile details
workbench config delete --profile old     # Delete a profile (cannot delete active)
```

### 3. Command reference

```
workbench
├── exec             # Execute remote command (non-interactive, millisecond-level)
├── upload           # Upload local file to instance (up to 1GB, via OSS relay)
├── download         # Download file from instance (up to 1GB, via OSS relay)
├── list             # List ECS instances
├── session          # Session management (list / close)
├── daemon           # Daemon lifecycle (start / status / stop)
├── config           # Credential configuration & profile management
│   ├── set          # Set a single config field
│   ├── list         # List all profiles
│   ├── switch       # Switch active profile
│   ├── get          # Show profile details
│   └── delete       # Delete a profile
├── upgrade          # Self-update
└── version          # Print version info
```

**Global flags:**

| Flag | Purpose | Default |
| --- | --- | --- |
| `--output` / `-o` | Output format: `text|json` | `text` |
| `--region` / `-r` | Alibaba Cloud region (e.g., `cn-hangzhou`) | auto-inferred from instance ID prefix |
| `--profile` / `-P` | Use a specific profile (overrides active profile) | current active profile |

### 4. List instances

```bash
workbench list ecs --region cn-hangzhou
workbench list ecs --region cn-hangzhou --status Running
workbench list ecs --region cn-hangzhou --tag env=prod --tag team=infra
workbench list ecs --region cn-hangzhou --instance-type ecs.g7.large
workbench list ecs --region cn-hangzhou --instance-name my-instance
workbench list ecs --region cn-hangzhou --image-id ubuntu_22_04_x64_20G_alibase_20230907.vhd
workbench list ecs --region cn-hangzhou --output json
```

`--region` is **required** for `list ecs`. Filters: `--status` (Running|Stopped|Starting|Stopping), `--tag` (key=value or key, repeatable, AND logic), `--instance-type` (e.g. ecs.g7.large), `--instance-name` (supports wildcards `*`), `--image-id`, `--vpc-id`, `--zone-id`, `--vswitch-id`, `--private-ip` (comma-separated), `--limit` (1-100, default 50), `--next-token` (pagination).

JSON output schema:

```json
[{
  "instance_id": "i-bp1xxxxx",
  "instance_name": "web-prod-01",
  "instance_type": "ecs.g7.large",
  "region_id": "cn-hangzhou",
  "status": "Running",
  "private_ip": "172.16.0.10",
  "public_ip": "",
  "os_type": "linux",
  "image_id": "ubuntu_22_04_x64_20G_alibase_20230907.vhd",
  "tags": {"env": "prod"}
}]
```

### 5. Remote command execution

**Built-in safety**: The CLI is a non-interactive executor — it does NOT provide a persistent shell. Each invocation is isolated and stateless, which prevents accidental cascading damage from lingering shell sessions.

**Agent pre-check requirement**: Before executing destructive commands (e.g., `rm -rf`, `shutdown`, `reboot`, `mkfs`, `dd`, service stop/restart, or any command that deletes data or halts the system), the Agent MUST confirm with the user by describing the intended action, the target instance, and the potential impact. Do NOT execute destructive commands without explicit user approval.

```bash
workbench exec --instance-id i-bp1xxxxx --command "df -h"
workbench exec --instance-id i-bp1xxxxx --command "sleep 30" --timeout 10
workbench exec --instance-id i-bp1xxxxx --command "df -h" --output json
```

| Flag | Required | Description | Default |
| --- | --- | --- | --- |
| `--instance-id` / `-i` | Yes | ECS instance ID | — |
| `--command` / `-c` | Yes | Command to execute | — |
| `--timeout` | No | Timeout in seconds | `30` |

**Important**: Each `exec` invocation runs in an independent shell context. State (cd, export) is NOT preserved between calls. Use `&&` or `;` to chain commands that need shared context in a single invocation.

JSON output schema:

```json
{
  "output": "Filesystem ...\n",
  "stderr": "",
  "exit_code": 0
}
```

### 6. File transfer

**Built-in safety**: The `upload` command has a built-in overwrite protection. When the remote destination file already exists, the CLI prompts for confirmation before overwriting:

```
Remote file "/root/id.txt" already exists (728 B, modified Jul 27 10:42). Overwrite? [y/N]
```

The default answer is **No** — if the user does not explicitly confirm, the upload is aborted. This prevents accidental overwriting of existing remote files.

**Agent pre-check requirement**: When using `upload` in automated/Agent workflows where interactive confirmation is not possible, the Agent MUST first check whether the target file exists on the remote instance (e.g., via `workbench exec --command "ls -la <path>"`) and inform the user if a file will be overwritten.

```bash
workbench upload ./app.jar /opt/app/app.jar --instance-id i-bp1xxxxx
workbench download /var/log/app.log ./ --instance-id i-bp1xxxxx
workbench download /var/log/app.log /tmp/local-copy.log --instance-id i-bp1xxxxx
```

Transfer goes through OSS as intermediary — transparent to the user, no OSS configuration needed. `download` second arg (local-path) is optional, defaults to current directory.

### 7. Session management

```bash
workbench session list
workbench session list --output json
workbench session close <session-id>
workbench session close --all
```

**Session lifecycle**: OPEN → RECONNECTING → BROKEN → CLOSED. Idle timeout 30min → auto CLOSED. Multiple operations on same instance share one session (transparent multiplexing).

### 8. Daemon management

```bash
workbench daemon start      # Manual start (usually not needed)
workbench daemon status
workbench daemon stop       # Closes all sessions
```

- **Auto-start**: Spawned on first CLI invocation.
- **Auto-exit**: 60 seconds after last session closes.
- **Singleton**: One per OS user (PID file lock).
- **IPC**: JSON-RPC over Unix socket (`~/.workbench/run/daemon.sock`).

### 9. Region inference

The CLI auto-infers region from the instance ID 3-character prefix (covers 290+ regions). If inference fails, `--region` is required. For `list` command, `--region` is always required.

## Exit Codes

| Code | Constant | Trigger |
| --- | --- | --- |
| **0** | `ExitSuccess` | Successful execution |
| **1** | `ExitGeneral` | Unclassified runtime error (includes instance not found, API errors, etc.) |
| **2** | `ExitArgument` | Missing, malformed, or invalid flag value |
| **3** | `ExitSessionNotFound` | Session ID invalid or expired |
| **4** | `ExitAuth` | Authentication or authorization failed |
| **5** | `ExitNetwork` | Network timeout, WebSocket exception |
| **6** | `ExitDaemonUnreach` | Local daemon not running or socket invalid |
| **7** | `ExitSessionBusy` | Session attached by another TTY |
| **N** | *(exec only)* | `exec` transparently passes through the remote command's exit code |

Error JSON output (on failure with `--output json`):

```json
{
  "code": 1,
  "message": "InvalidParameter.InstanceId: the specified instance does not exist"
}
```

## RAM Permissions

Minimum RAM policy required:

```json
{
  "Version": "1",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "ecs-workbench:*"
      ],
      "Resource": "*"
    },
    {
      "Effect": "Allow",
      "Action": [
        "ecs:DescribeInstances",
        "ecs:DescribeCloudAssistantStatus",
        "ecs:StartTerminalSession"
      ],
      "Resource": "*"
    },
    {
      "Effect": "Allow",
      "Action": "ram:CreateServiceLinkedRole",
      "Resource": "*",
      "Condition": {
        "StringEquals": {
          "ram:ServiceName": "workbench.ecs.aliyuncs.com"
        }
      }
    }
  ]
}
```

Restrict to specific instances: replace `"Resource": "*"` in each statement with the instance ARN:
- `acs:ecs:<region>:<account-id>:instance/<instance-id>`

## Troubleshooting

| Error message pattern | Exit Code | First Action |
| --- | --- | --- |
| `InvalidAccessKeyId` / authentication errors | 4 | Verify AK/SK in `~/.workbench/config.json`. Re-write it with `wb-config.sh set ...` (or `/alibabacloud-workbench:setup`), then `wb-config.sh verify`. |
| `profile not found` | 1 | Check profile name with `workbench config list`. |
| `not found in <region>` / instance not found | 1 | Verify instance ID and region. Use `workbench list --region <region>` to confirm. |
| Network timeout / WebSocket errors | 5 | Check network connectivity to `*.aliyuncs.com`. Verify security group rules. |
| Session busy / attach errors | 7 | Another terminal is attached. Close it first, or use `workbench session close <id>`. |
| `cannot start daemon` / socket errors | 6 | Run `workbench daemon status`. If stopped, any command will auto-restart it. |
| Permission denied (RAM) | 4 | Attach the RAM policy from `## RAM Permissions` to the user or role. |
| `command not found: workbench` | 127 | Not installed or not on `PATH`. Run `bash "${QODER_PLUGIN_ROOT}/scripts/install-cli.sh"`, check `${QODER_PLUGIN_DATA}/bootstrap.log`, or use the absolute path from `wb-config.sh check`. |
| `insecure permissions` | 2 | Run `chmod 600 ~/.workbench/config.json`. |
| STS token expired | 4 | If using `RamRoleArn` mode, the CLI auto-refreshes. If using static STS, update the token. |
