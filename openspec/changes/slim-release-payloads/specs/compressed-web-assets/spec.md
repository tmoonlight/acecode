## Purpose

Reduce installed application size and static-resource transfer costs while preserving the complete offline frontend, existing URLs, MIME types, cache behavior, and development directory overrides.

## ADDED Requirements

### Requirement: Lossless compressed frontend delivery
The packaged application SHALL store compressible frontend assets in compressed form when that reduces size, and SHALL deliver their complete original content with correct content types. Generation MUST be deterministic and preserve binary bytes and version substitution.

#### Scenario: Browser accepts gzip
- **WHEN** a browser requests a compressed embedded asset with gzip accepted
- **THEN** the response carries gzip encoding and an appropriate Vary header, and decompresses to the original asset

#### Scenario: Client does not accept gzip
- **WHEN** a client omits gzip support or explicitly assigns gzip quality zero
- **THEN** the same resource is returned uncompressed without a Content-Encoding header

#### Scenario: Existing asset changes
- **WHEN** a frontend file is modified without changing its name
- **THEN** the next native build refreshes its embedded content

#### Scenario: Development directory and incompressible assets
- **WHEN** an asset comes from the configured development directory or compression would increase its size
- **THEN** its original bytes remain available through the same resource URL
