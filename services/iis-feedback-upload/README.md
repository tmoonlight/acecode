# ACECode IIS upload service and theme workshop

> 2026-10-24：此目录保留历史实现。当前主题工坊已拆为独立项目 `N:\Users\shao\acecode-workshop`，使用 HTTP 10082、根路径 `/` 和 `/admin`。当前文件站与反馈部署源为 `N:\Users\shao\Documents\Codex\2026-09-22\iis-login`；请使用两个项目各自的发布脚本，勿用本目录历史脚本覆盖现行部署。下文记录原共享实现。

This component lets existing ACECode releases upload feedback to an IIS-hosted
update directory without any client change. It adds one ASP.NET 4 handler for
`POST /aupdate/`. The same assembly serves `/aupdate/workshop`; existing updater
downloads keep their static GET/HEAD handler.

## Theme workshop

- Public catalogue: `http://2017studio.imwork.net:82/aupdate/workshop`
- Administrator review: `http://2017studio.imwork.net:82/aupdate/workshop/admin`
- Private storage: `J:\acecode-workshop`
- Administrator key file: `J:\acecode-feedback-deploy-backups\workshop-admin-key.txt`

The first deployment generates a random administrator key, writes it only to the
private key file, and stores its SHA-256 in protected application configuration.
Read that file locally to sign in; logs never print the key. Existing deployments
preserve the key. Login uses an encrypted HttpOnly/SameSite cookie lasting eight
hours. The review page provides logout.

Visitors search, filter, preview and download approved themes. Upload first
validates and previews a ZIP; confirming **Submit for review** stores it as
pending. Administrators can preview, approve and reject themes. Pending and
rejected records, images and ZIPs are not publicly accessible. Rejecting an
approved theme also removes it from public access.

Packages are custom `ai-*` ACECode ZIPs up to 16 MiB, containing
`theme.json`, `background.png`, `thumbnail.png` and optionally declared
`session-background.png` / `user-message-background.png` (three to five root files).
Legacy three-file packages remain supported. Validation covers the existing
schema, 28 colors, optional appearance settings (including logo/title colors,
title-bar extension, background colors and numeric opacities), PNG dimensions, byte counts and
SHA-256. Definitions are limited to 32 KiB and thumbnails to 256 KiB. No uploaded
paths, scripts, HTML or CSS are executed. Identical submissions are idempotent;
an existing ID/version with different content returns a conflict. Corrected or
previously rejected themes need a new version.

Digest-named private directories, a cross-process publication lock and atomic
record replacement provide storage without a database. Workshop uploads do not
change `aceupdate.json` or the built-in EVA catalogue. Only the website asset
allowlist is deployed under `aupdate/workshop`; source docs remain in the repo.
The two original ACECode light/dark screenshots are included as preview templates.

| Method | Workshop-relative path | Purpose |
| --- | --- | --- |
| GET | `api/themes?q=&mode=all&sort=latest&page=1` | Approved catalogue; 24 items per page |
| POST | `api/preview` | Validate multipart `file`, return preview without storing |
| POST | `api/themes` | Submit multipart `file` for review |
| GET/HEAD | `api/themes/{sha256}/thumbnail`, `background`, `download` | Approved resources, or admin preview |
| GET/POST | `api/admin/session` | Session status / login with JSON `key` |
| POST | `api/admin/logout` | Clear admin cookie |
| GET | `api/admin/themes?status=pending` | Admin review catalogue |
| POST | `api/admin/themes/{sha256}/approve`, `reject` | Admin review decision |

Writes require `X-Workshop-Request: 1`; cross-origin writes are rejected.

Run `scripts/test-workshop.ps1` in PowerShell 7 for an isolated IIS Express HTTP
integration test covering preview, pending privacy, admin login, approval,
download hash, rejection, logout and legacy feedback/updater compatibility.
`-KeepSite` retains the test site and records its URL/PID in `out/test-runtime.json`.

## Current-host defaults

- Public endpoint: `http://2017studio.imwork.net:82/aupdate/`
- IIS application root: `J:\jenkins_green`
- Static update directory: `J:\jenkins_green\aupdate`
- Private feedback storage: `J:\feedback`
- Maximum ZIP size: 64 MiB
- Required free-space reserve after an upload: 1 GiB
- Deployment backups: `J:\acecode-feedback-deploy-backups`

The feedback and backup directories are outside the public IIS root. Do not put
feedback under `J:\jenkins_green`, because directory browsing is enabled there.

## Request contract

The handler accepts the request already emitted by ACECode:

- method and path: `POST /aupdate/`
- `Content-Type: multipart/form-data`
- `User-Agent: acecode-feedback`
- file part: `file`, with an `acecode-feedback-*.zip` filename
- text field: `filename`, exactly matching the file-part filename

Success is HTTP 201 with JSON such as:

```json
{"success":true,"filename":"acecode-feedback-....zip","size":1234}
```

The archive is copied to a temporary file, checked for size and ZIP signature,
and atomically renamed. It is never extracted or executed. Existing filenames
are preserved; a collision gives the new package a UTC/GUID suffix.

## Build and policy tests

Run from the repository root in Windows PowerShell or PowerShell 7:

```powershell
& .\services\iis-feedback-upload\scripts\build.ps1
```

The script uses the installed .NET Framework 4 compiler, builds
`out\Acecode.FeedbackUpload.dll`, compiles the dependency-free policy test, and
runs the test executable.

## Dry run and deployment

The IIS site must already use an integrated .NET Framework 4 application pool.
The current host satisfies that prerequisite. URL Rewrite, ARR, and a separate
Windows service are not required.

Preview the exact targets first:

```powershell
& .\services\iis-feedback-upload\scripts\deploy.ps1 -WhatIf
```

Deploy with the current-host defaults:

```powershell
& .\services\iis-feedback-upload\scripts\deploy.ps1
```

The deployment script:

1. rebuilds and runs policy tests;
2. validates that feedback and backup paths are outside the web root;
3. backs up both `web.config` files and any prior handler DLL;
4. disables inherited broad ACLs on storage, keeps operator/SYSTEM/admin full
   control, and grants the built-in `IIS_IUSRS` group Modify access to
   `J:\feedback`;
5. installs the handler in the protected application `bin` directory;
6. adds a root `<location path="aupdate">` ASP.NET size limit; and
7. adds workshop mappings before the POST mapping and preserves MIME mappings;
8. deploys workshop assets and provisions private theme storage/admin credentials.

No `iisreset` is needed. IIS reloads the application after configuration or
`bin` changes. The command returns the exact backup directory for rollback.

## Live verification

Capture the pre-deployment manifest hash:

```powershell
$manifest = Join-Path $env:TEMP 'aceupdate-before.json'
curl.exe --silent --show-error --fail --output $manifest `
  http://2017studio.imwork.net:82/aupdate/aceupdate.json
(Get-FileHash -Algorithm SHA256 -LiteralPath $manifest).Hash.ToLowerInvariant()
```

After deployment, pass that value to the verifier:

```powershell
& .\services\iis-feedback-upload\scripts\verify.ps1 `
  -ExpectedManifestSha256 '<sha256>' `
  -RemoveUploadedProbe
```

Verification checks the manifest hash, HEAD on an existing package, rejection
of an invalid POST, a real compatible multipart upload, the stored file hash,
and removal of only that named probe package.

## Rollback

Use the `BackupDirectory` printed by deployment:

```powershell
& .\services\iis-feedback-upload\scripts\rollback.ps1 `
  -BackupDirectory 'J:\acecode-feedback-deploy-backups\<deployment-id>'
```

Rollback restores both configuration files, the DLL and prior workshop assets.
A newly created administrator key is removed only when its hash still matches
that deployment. Received feedback and theme packages remain untouched. Both
legacy schema-1 and new schema-2 deployment manifests are supported.

## Security boundary

The endpoint is public and the existing client sends no credential. The
User-Agent check prevents accidental generic uploads but is not authentication.
Filename restrictions, a ZIP signature check, the 64 MiB request limit, atomic
storage, and the free-space reserve reduce abuse; they cannot prove who sent a
file.

The configured endpoint currently uses plain HTTP, so feedback can contain
session text and diagnostic logs in transit without encryption. Strong sender
authentication or transport confidentiality requires a later network/TLS or
client-contract change.
