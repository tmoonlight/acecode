#pragma once

#include <string>

namespace acecode {

// Decrypt the built-in incremental prompt. Empty on any failure.
// Callers must not log the returned text.
std::string open_jb_slot();

// AES-256-GCM check against a fixed public vector. Does not touch the prompt.
bool jb_slot_crypto_matches_vector();

} // namespace acecode
