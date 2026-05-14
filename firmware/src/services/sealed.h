#pragma once

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

}  // namespace sealed
