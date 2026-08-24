# ACECode IIS feedback upload compatibility

This component lets existing ACECode releases upload feedback to an IIS-hosted
update directory without any client change. It adds one ASP.NET 4 handler for
`POST /aupdate/`; GET and HEAD requests remain static and read-only.

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
7. adds the POST mapping and IIS request limit under `aupdate` while preserving
   all existing MIME mappings.

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

Rollback restores both prior IIS configuration files and either restores or
removes the handler DLL according to the manifest. It deliberately leaves
`J:\feedback` and all received feedback packages untouched.

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
