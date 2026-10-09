## ADDED Requirements

### Requirement: ACEModel configurable Responses default

ACECode SHALL default OpenAI runtime profiles identified by catalog provider `acemodel` to Responses when `api_protocol` is omitted. Explicit `chat_completions` or `responses` selections MUST take precedence. This default SHALL apply consistently to new profiles, loaded legacy profiles, runtime requests, connection tests and Windows installer presets, independently of model names, endpoint URLs and capability overrides. Other providers SHALL retain their existing default protocol.

#### Scenario: Create ACEModel profiles
- **WHEN** a user selects ACEModel and one or multiple models in model settings
- **THEN** the API protocol selector defaults to Responses and saving preserves that protocol on every created profile

#### Scenario: Load a legacy ACEModel profile
- **WHEN** an OpenAI profile with catalog identity `acemodel` has no explicit API protocol
- **THEN** the runtime and model settings use Responses, including profiles with manual capabilities or a proxy endpoint

#### Scenario: Change and retain a protocol selection
- **WHEN** the user selects Chat Completions or Responses in an ACEModel profile's API protocol selector and saves it
- **THEN** inference, connection tests and subsequent edits use the saved selection, and loading or installer upgrades preserve it

#### Scenario: Clear an override
- **WHEN** an API update clears an ACEModel profile's explicit protocol with `null`
- **THEN** its effective protocol returns to the ACEModel default of Responses

#### Scenario: Switch providers
- **WHEN** a user switches the model form between ACEModel and another provider
- **THEN** the new provider's default replaces the preceding protocol selection without carrying Responses into other providers implicitly

#### Scenario: Seed installer profiles
- **WHEN** the Windows installer creates ACEModel presets or upgrades presets without an explicit protocol
- **THEN** it writes Responses and leaves existing explicit protocol choices and unrelated profiles intact
