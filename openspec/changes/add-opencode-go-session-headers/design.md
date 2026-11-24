## Context

Go identifies a coding client by its User-Agent and routes each conversation by x-opencode-session. The models.dev JSON exposes the provider ID and endpoint. The dynamic session value belongs to the request.

## Decisions

- Select the policy by models_dev_provider_id or the official HTTPS Go inference and legacy Zen Go URL prefixes. A shared model name does not identify a provider.
- Resolve configured headers and merge provider defaults once before transport retries. Explicit header values win case-insensitively. Use the ACECode version for the default User-Agent.
- Reuse the request builder's existing stable session key for normal turns and compaction. Derive a header-safe opaque ID from it. Give detached side conversations a stable identity across their tool rounds and give standalone calls such as connection tests fresh IDs.
- Carry the policy in the provider construction fingerprint so a changed profile is re-bound. Do not persist runtime headers or mutate shared providers with a current session.

## Verification

Use a local HTTP server to capture inference headers, check independent sessions sharing a provider and exercise the settings connection-test handler. Cover Go endpoint recognition and explicit overrides.
