#include "identity.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <libssh/libssh.h>
#include <libssh_esp32.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>

#include <vector>

namespace {

std::string g_pub;
std::string g_priv;
std::string g_fp;
uint8_t     g_sealKey[32]   = {0};
std::string g_sealKeyB64;
bool        g_ready = false;

std::string macSuffix() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    return buf;
}

std::string sha256Fingerprint(const std::string& openSshPub) {
    // OpenSSH-format pubkey is "ssh-ed25519 <base64-of-wireformat> [comment]".
    // The fingerprint is over the base64-decoded wireformat blob.
    size_t a = openSshPub.find(' ');
    if (a == std::string::npos) return "";
    size_t b = openSshPub.find(' ', a + 1);
    std::string b64 = (b == std::string::npos)
                          ? openSshPub.substr(a + 1)
                          : openSshPub.substr(a + 1, b - a - 1);

    std::vector<uint8_t> raw(b64.size());
    size_t out_len = 0;
    if (mbedtls_base64_decode(raw.data(), raw.size(), &out_len,
                              reinterpret_cast<const uint8_t*>(b64.data()),
                              b64.size()) != 0) {
        return "";
    }

    uint8_t hash[32];
    mbedtls_sha256(raw.data(), out_len, hash, 0);

    // OpenSSH-style: base64-encoded SHA256 with trailing '=' stripped.
    char fpBuf[64];
    size_t enc_len = 0;
    mbedtls_base64_encode(reinterpret_cast<uint8_t*>(fpBuf), sizeof(fpBuf),
                          &enc_len, hash, sizeof(hash));
    std::string fp(fpBuf, enc_len);
    while (!fp.empty() && fp.back() == '=') fp.pop_back();
    return "SHA256:" + fp;
}

bool generate() {
    ssh_key key = nullptr;
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) {
        Serial.println("[identity] ssh_pki_generate failed");
        return false;
    }

    char* priv = nullptr;
    if (ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &priv) != SSH_OK) {
        Serial.println("[identity] export_privkey failed");
        ssh_key_free(key);
        return false;
    }
    char* pub = nullptr;
    if (ssh_pki_export_pubkey_base64(key, &pub) != SSH_OK) {
        Serial.println("[identity] export_pubkey failed");
        ssh_string_free_char(priv);
        ssh_key_free(key);
        return false;
    }

    std::string comment = std::string("clawdputer@") + macSuffix();
    g_priv = priv;
    g_pub  = std::string("ssh-ed25519 ") + pub + " " + comment;

    ssh_string_free_char(priv);
    ssh_string_free_char(pub);
    ssh_key_free(key);

    Preferences prefs;
    prefs.begin("identity", false);
    prefs.putString("ed25519_priv", g_priv.c_str());
    prefs.putString("ed25519_pub",  g_pub.c_str());
    prefs.end();

    return true;
}

}  // namespace

namespace identity {

void loadOrGenerateSealKey() {
    Preferences prefs;
    prefs.begin("identity", false);
    size_t n = prefs.getBytes("seal_key", g_sealKey, sizeof(g_sealKey));
    if (n != sizeof(g_sealKey)) {
        Serial.println("[identity] generating new seal key");
        for (size_t i = 0; i < sizeof(g_sealKey); i++) g_sealKey[i] = (uint8_t)esp_random();
        prefs.putBytes("seal_key", g_sealKey, sizeof(g_sealKey));
    } else {
        Serial.println("[identity] seal key loaded from NVS");
    }
    prefs.end();

    char b64[64];
    size_t enc_len = 0;
    mbedtls_base64_encode(reinterpret_cast<uint8_t*>(b64), sizeof(b64),
                          &enc_len, g_sealKey, sizeof(g_sealKey));
    g_sealKeyB64.assign(b64, enc_len);
}

void begin() {
    if (g_ready) return;

    Preferences prefs;
    prefs.begin("identity", true);
    String storedPriv = prefs.getString("ed25519_priv", "");
    String storedPub  = prefs.getString("ed25519_pub",  "");
    prefs.end();

    if (storedPriv.length() > 0 && storedPub.length() > 0) {
        g_priv = storedPriv.c_str();
        g_pub  = storedPub.c_str();
        Serial.println("[identity] keypair loaded from NVS");
    } else {
        Serial.println("[identity] generating new Ed25519 keypair");
        if (!generate()) {
            Serial.println("[identity] generation FAILED — SSH key auth unavailable");
            return;
        }
        Serial.println("[identity] keypair generated and saved");
    }

    g_fp = sha256Fingerprint(g_pub);
    loadOrGenerateSealKey();
    g_ready = true;
    Serial.printf("[identity] fingerprint: %s\n", g_fp.c_str());
}

std::string publicKeyOpenSsh() { return g_pub; }
std::string privateKeyPem()    { return g_priv; }
std::string fingerprint()      { return g_fp; }

const uint8_t* sealKey()         { return g_sealKey; }
std::string    sealKeyBase64()   { return g_sealKeyB64; }

}  // namespace identity
