## MODIFIED Requirements

### Requirement: Dedicated release family
Release packaging SHALL replace `linux-old` archives with `acecode-linux-deepin-<arch>.tar.gz` for x64 and arm64, both with Desktop coverage, and explicitly select the dedicated build variant. Deepin ARMv7 SHALL NOT be built or published after 1.0.7. Desktop packages MUST use WebKitGTK 4.0 and DTK 5; the maximum required GLIBC version SHALL remain 2.28.

#### Scenario: Complete release
- **WHEN** a tagged release is assembled
- **THEN** the x64 and arm64 Deepin archives are required along with the existing generic platform artifacts

#### Scenario: Retired ARMv7 archive appears
- **WHEN** the release artifacts contain `acecode-linux-deepin-armv7.tar.gz`
- **THEN** release verification rejects it as an unexpected asset and no Release is created

#### Scenario: Incompatible Desktop linkage
- **WHEN** the dedicated Desktop links WebKitGTK 4.1/6, lacks DTK 5 or requires GLIBC above 2.28
- **THEN** the packaging verification fails
