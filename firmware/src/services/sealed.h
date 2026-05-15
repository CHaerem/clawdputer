#pragma once

#include <string>
#include <vector>

#include "secrets/ssh_hosts.h"

namespace sealed {

// Decrypts the firmware-embedded sealed ssh_hosts blob using the per-device
// seal key from identity::sealKey(). Returns an empty vector if the blob is
// missing, malformed, or fails AEAD verification.
//
// The decrypted strings live in a thread-local arena owned by this module —
// safe to read for the lifetime of the call site (the SSH app stages just
// hold the pointers; on app exit / re-enter, call again).
std::vector<SshHost> unsealSshHosts();

// Decrypts the firmware-embedded sealed GitHub PAT (plaintext: a single
// token like "ghp_..."). Result is cached after the first successful
// call. Returns empty when the blob is absent, malformed, or fails AEAD
// verification (e.g. sealed against a different device's seal key).
std::string unsealGithubPat();

}  // namespace sealed
