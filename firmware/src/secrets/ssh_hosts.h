#pragma once

// SSH connection presets — SEALED.
//
// This file declares only the shape of an SshHost. The actual host list is
// encrypted (AES-256-GCM, per-device seal key) and committed under
// firmware/secrets/ssh_hosts.sealed. The pre-build script
// firmware/scripts/embed_secrets.py emits sealed_blobs.h with the raw
// bytes; SSH app calls sealed::unsealSshHosts() at runtime to decrypt.
//
// To add a host: edit tools/hosts.json, run tools/seal-hosts.py, commit
// the updated .sealed file. Plaintext hostnames/usernames never enter the
// repo.

struct SshHost {
    const char* name;
    const char* host;
    const char* user;
    int         port;
};
