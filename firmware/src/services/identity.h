#pragma once

#include <string>

// Device identity for SSH key-based auth.
//
// A Ed25519 keypair is generated on first boot and persisted in NVS. The
// public half can be displayed (Settings → Show SSH pubkey) and copied to
// the target hosts' `~/.ssh/authorized_keys`. The private half never leaves
// the device — it's loaded into a libssh session at connect time.

namespace identity {

void begin();

// Public key in OpenSSH format: "ssh-ed25519 AAAA... clawdputer@<mac>".
// Safe to display, log, or commit to a repo as authorized_keys content.
std::string publicKeyOpenSsh();

// Private key in OpenSSH PEM form, suitable for ssh_pki_import_privkey_base64.
// Treat as a secret — should never be displayed or transmitted.
std::string privateKeyPem();

// "SHA256:abc…" fingerprint — short identifier for the UI.
std::string fingerprint();

// AES-256 seal key, generated alongside the Ed25519 keypair on first boot.
// Used to decrypt firmware/secrets/*.sealed at runtime. Same key is also
// shown (base64) in Settings → "show seal key" so the user can encrypt
// new secrets on the Mac with tools/seal-hosts.py and commit the result.
const uint8_t* sealKey();        // 32 bytes
std::string    sealKeyBase64();  // for the Settings UI

}  // namespace identity
