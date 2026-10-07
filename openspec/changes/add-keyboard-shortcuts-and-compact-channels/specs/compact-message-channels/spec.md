## Purpose

Provide recognizable message-channel cards whose actions remain in stable positions throughout setup, connection and approval changes while preserving access to operational details and recovery.

## ADDED Requirements

### Requirement: Stable channel cards
Cards SHALL show management immediately left of the connection action. An enabled channel SHALL offer 取消连接, including while connecting or retrying. Configuration, connection status and pending approvals SHALL NOT add rows or increase card height.

#### Scenario: State transitions
- **WHEN** a channel becomes configured, connects, fails or receives pending approval requests
- **THEN** its card keeps the same height and button positions at the same viewport, and status remains accessible

### Requirement: Management retains detail and recovery
Management SHALL expose status, connection recovery, bot configuration, pending approval decisions and access management. Existing close-button dialogs SHALL keep their backdrop dismissal policy.

#### Scenario: Approve from management
- **WHEN** a channel has a pending request
- **THEN** its management entry indicates pending work and the user can open it to approve or reject the request

### Requirement: Local platform identity
QQ, WeChat, Feishu, DingTalk, Telegram, Discord and LINE SHALL display recognizable locally bundled vendor icons in their cards and dialogs, across light and dark themes.

#### Scenario: Offline assets
- **WHEN** the settings page loads without access to a public asset CDN
- **THEN** all seven platform icons still render from the application bundle
