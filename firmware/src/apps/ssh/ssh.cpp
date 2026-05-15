// SSH client — native libssh over WiFi with Ed25519 public-key auth.
//
// Auth uses the device identity (services/identity.h) — a per-device
// Ed25519 keypair generated on first boot and persisted in NVS. The
// private key never leaves the device; the public half is added to each
// target host's ~/.ssh/authorized_keys (one-time copy, surfaced in
// Settings → "Show SSH pubkey").
//
// Hosts are picked from secrets/ssh_hosts.h, which is committed to the
// repo as plain text (hostname/user are not sensitive). PR to add a host,
// push, GitOps deploys.
//
// Known-hosts handling: trust-on-first-use for now. Acceptable on a
// trusted LAN; revisit before production.

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <libssh/libssh.h>
#include <libssh_esp32.h>

#include <deque>
#include <string>

#include "core/app.h"
#include "core/key.h"
#include "secrets/ssh_hosts.h"
#include "services/bridge.h"
#include "services/identity.h"
#include "services/sealed.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

enum class Stage : uint8_t {
    Picker,        // choose a sealed/saved host or "+ new"
    Form,          // type host/user for an ad-hoc connect
    Connecting,
    Connected,
    AskSave,       // after first ad-hoc success: persist to NVS?
    Error,
};

enum class FormField : uint8_t { Host, User };

Stage                g_stage    = Stage::Picker;
int                  g_selected = 0;
std::string          g_status;
bool                 g_dirty    = true;
std::vector<SshHost> g_hosts;          // sealed presets
std::vector<SshHost> g_savedHosts;     // NVS-stored ad-hoc adds
std::vector<std::string> g_savedStrings;  // backing storage for g_savedHosts

// Ad-hoc form state
FormField   g_formField  = FormField::Host;
std::string g_formHost;
std::string g_formUser;
bool        g_adhocConnected = false;  // true → AskSave applies on disconnect

std::deque<std::string> g_lines;
std::string             g_input;
ssh_session             g_session = nullptr;
ssh_channel             g_channel = nullptr;
SshHost                 g_active  = {};

// libssh's handshake nests through mbedTLS and easily blows past Arduino's
// 8 KB loopTask stack. We run the blocking handshake on a dedicated 24 KB
// task pinned to Core 1 (loopTask is on Core 1; NimBLE on Core 0, so the
// move keeps each subsystem's stack pressure separate). The worker writes
// a result code via the volatile flags below; main thread polls them.
TaskHandle_t      g_sshTask           = nullptr;
volatile bool     g_handshakeDone     = false;
volatile bool     g_handshakeOk       = false;
SshHost           g_pending           = {};
char              g_pendingHostBuf[64] = {0};
char              g_pendingUserBuf[64] = {0};

constexpr size_t MAX_LINES = 100;

const char* intern(const std::string& s) {
    g_savedStrings.push_back(s);
    return g_savedStrings.back().c_str();
}

void loadSavedHosts() {
    g_savedHosts.clear();
    g_savedStrings.clear();

    Preferences prefs;
    prefs.begin("ssh", true);
    int n = prefs.getInt("count", 0);
    for (int i = 0; i < n; i++) {
        char key[16];
        snprintf(key, sizeof(key), "h%d", i);
        String blob = prefs.getString(key, "");
        if (blob.length() == 0) continue;
        // Stored as "host\tuser\tport" — simple to parse, no JSON dependency.
        std::string s = blob.c_str();
        size_t a = s.find('\t');
        size_t b = s.find('\t', a == std::string::npos ? 0 : a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        std::string host = s.substr(0, a);
        std::string user = s.substr(a + 1, b - a - 1);
        int         port = atoi(s.substr(b + 1).c_str());
        SshHost h;
        h.name = intern(host);  // display the host as the name
        h.host = g_savedStrings.back().c_str();
        h.user = intern(user);
        h.port = port > 0 ? port : 22;
        g_savedHosts.push_back(h);
    }
    prefs.end();
}

void saveHost(const SshHost& h) {
    Preferences prefs;
    prefs.begin("ssh", false);
    int n = prefs.getInt("count", 0);
    char key[16];
    snprintf(key, sizeof(key), "h%d", n);
    char blob[128];
    snprintf(blob, sizeof(blob), "%s\t%s\t%d", h.host, h.user, h.port);
    prefs.putString(key, blob);
    prefs.putInt("count", n + 1);
    prefs.end();
    Serial.printf("[ssh] saved host %s@%s\n", h.user, h.host);
}

void pushLine(const std::string& s) {
    g_lines.push_back(s);
    while (g_lines.size() > MAX_LINES) g_lines.pop_front();
    g_dirty = true;
}

void teardown() {
    if (g_channel) {
        ssh_channel_close(g_channel);
        ssh_channel_free(g_channel);
        g_channel = nullptr;
    }
    if (g_session) {
        ssh_disconnect(g_session);
        ssh_free(g_session);
        g_session = nullptr;
    }
    g_active = {};
    bridge::resume();
}

bool doConnectBlocking(const SshHost& h);

void sshConnectTask(void* arg) {
    SshHost* h = static_cast<SshHost*>(arg);
    g_handshakeOk   = doConnectBlocking(*h);
    g_handshakeDone = true;
    vTaskDelete(nullptr);
}

// Kick off the worker. Returns immediately; main loop polls g_handshakeDone.
bool startConnection(const SshHost& h) {
    // libssh + mbedTLS scratch needs ~50 KB heap; refuse early if tight.
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 60000) {
        char buf[64];
        snprintf(buf, sizeof(buf), "low memory (%u bytes free)", (unsigned)freeHeap);
        g_status = buf;
        return false;
    }

    // Copy host/user into stable storage — kSshHosts pointers come from
    // firmware rodata but g_savedHosts entries point into a vector that
    // may move while the worker is running.
    strncpy(g_pendingHostBuf, h.host ? h.host : "", sizeof(g_pendingHostBuf) - 1);
    strncpy(g_pendingUserBuf, h.user ? h.user : "", sizeof(g_pendingUserBuf) - 1);
    g_pending      = h;
    g_pending.host = g_pendingHostBuf;
    g_pending.user = g_pendingUserBuf;

    bridge::pause();          // free mbedTLS heap for the worker
    g_handshakeDone = false;
    g_handshakeOk   = false;
    BaseType_t rc = xTaskCreatePinnedToCore(
        sshConnectTask, "ssh-connect", 24576, &g_pending, 1, &g_sshTask, 1);
    if (rc != pdPASS) {
        g_status = "could not start ssh task";
        bridge::resume();
        return false;
    }
    return true;
}

bool doConnectBlocking(const SshHost& h) {
    g_session = ssh_new();
    if (!g_session) { g_status = "ssh_new failed"; return false; }

    int port    = h.port > 0 ? h.port : 22;
    int verbose = SSH_LOG_NOLOG;
    ssh_options_set(g_session, SSH_OPTIONS_HOST,    h.host);
    ssh_options_set(g_session, SSH_OPTIONS_USER,    h.user);
    ssh_options_set(g_session, SSH_OPTIONS_PORT,    &port);
    ssh_options_set(g_session, SSH_OPTIONS_LOG_VERBOSITY, &verbose);
    // Bound the handshake so a stuck host can't wedge the loop forever.
    int timeoutSec = 8;
    ssh_options_set(g_session, SSH_OPTIONS_TIMEOUT, &timeoutSec);

    Serial.printf("[ssh] connect %s@%s:%d (free heap %u)\n",
                  h.user, h.host, port, (unsigned)ESP.getFreeHeap());

    if (ssh_connect(g_session) != SSH_OK) {
        g_status = std::string("connect: ") + ssh_get_error(g_session);
        return false;
    }

    // Import our Ed25519 private key from NVS and authenticate with it.
    ssh_key privkey = nullptr;
    std::string pem = identity::privateKeyPem();
    if (pem.empty()) { g_status = "no device key"; return false; }
    if (ssh_pki_import_privkey_base64(pem.c_str(), nullptr, nullptr, nullptr, &privkey) != SSH_OK) {
        g_status = "key import failed";
        return false;
    }
    int rc = ssh_userauth_publickey(g_session, nullptr, privkey);
    ssh_key_free(privkey);
    if (rc != SSH_AUTH_SUCCESS) {
        g_status = std::string("auth: ") + ssh_get_error(g_session);
        return false;
    }

    g_channel = ssh_channel_new(g_session);
    if (!g_channel) { g_status = "channel_new failed"; return false; }
    if (ssh_channel_open_session(g_channel) != SSH_OK) {
        g_status = std::string("open: ") + ssh_get_error(g_session);
        return false;
    }
    // Allocate a pseudo-terminal so commands like sudo (which refuse to
    // prompt without a tty) work. Size matches what the chat-style line
    // viewport can render comfortably; xterm is the safest TERM string.
    if (ssh_channel_request_pty_size(g_channel, "xterm", 38, 12) != SSH_OK) {
        g_status = std::string("pty: ") + ssh_get_error(g_session);
        return false;
    }
    if (ssh_channel_request_shell(g_channel) != SSH_OK) {
        g_status = std::string("shell: ") + ssh_get_error(g_session);
        return false;
    }
    ssh_channel_set_blocking(g_channel, 0);
    Serial.printf("[ssh] connected (free heap %u)\n", (unsigned)ESP.getFreeHeap());
    return true;
}

// Strip ANSI escape sequences (CSI/OSC) inline and accumulate clean bytes.
void appendStripped(std::string& dst, const char* src, size_t n) {
    static int      esc      = 0;   // ANSI parser state: 0=normal, 1=saw ESC, 2=inside CSI
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (esc == 1) {
            if (c == '[' || c == '(' || c == ')') esc = 2;
            else                                   esc = 0;
            continue;
        }
        if (esc == 2) {
            if ((c >= '@' && c <= '~')) esc = 0;  // terminator byte
            continue;
        }
        if (c == 0x1B) { esc = 1; continue; }
        if (c == '\r')               continue;
        if (c == '\b' || c == 0x7F) {
            if (!dst.empty() && dst.back() != '\n') dst.pop_back();
            continue;
        }
        if (c == '\n' || (c >= 0x20 && c <= 0x7E)) dst.push_back(c);
    }
}

void pollChannel() {
    if (!g_channel) return;

    char buf[256];
    int  n;
    static std::string carry;
    bool gotData = false;

    while ((n = ssh_channel_read_nonblocking(g_channel, buf, sizeof(buf) - 1, 0)) > 0) {
        appendStripped(carry, buf, (size_t)n);
        gotData = true;
        size_t start = 0;
        for (size_t i = 0; i < carry.size(); i++) {
            if (carry[i] == '\n') {
                pushLine(carry.substr(start, i - start));
                start = i + 1;
            }
        }
        carry.erase(0, start);
    }

    // Any trailing partial line (no newline yet) is shown as the last row so
    // password prompts and similar partial output become visible.
    if (gotData) {
        if (!g_lines.empty() && !g_lines.back().empty() && g_lines.back().back() == '\x01') {
            // marker: previous tail-row — replace it
            g_lines.pop_back();
        }
        if (!carry.empty()) {
            // Tag with sentinel so we replace it on the next poll.
            pushLine(carry + std::string(1, '\x01'));
        }
        g_dirty = true;
    }

    if (ssh_channel_is_eof(g_channel)) {
        pushLine("[remote closed]");
        teardown();
        g_stage  = Stage::Error;
        g_status = "disconnected";
        g_dirty  = true;
    }
}

void sendCommand() {
    if (!g_channel) return;
    std::string cmd = g_input + "\n";
    ssh_channel_write(g_channel, cmd.c_str(), cmd.size());
    pushLine(std::string("$ ") + g_input);
    g_input.clear();
    g_dirty = true;
}

// ----------- rendering -----------

void drawFooter(const char* hint) {
    auto& d = ui::display();
    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print(hint);
}

void renderPicker() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 3);
    d.print("SSH hosts");

    // Combined list: sealed presets + NVS-saved hosts + "+ new host"
    auto allCount = (int)g_hosts.size() + (int)g_savedHosts.size() + 1;
    int  y        = ui::statusbar::HEIGHT + 16;
    int  rowH     = 13;
    int  maxRows  = (110 - y) / rowH;
    int  scrollTop = 0;
    if (g_selected >= scrollTop + maxRows) scrollTop = g_selected - maxRows + 1;

    for (int i = scrollTop; i < allCount && i < scrollTop + maxRows; i++) {
        bool sel = i == g_selected;
        if (sel) d.fillRoundRect(2, y - 1, SCREEN_W - 4, rowH, 2, 0x18E3);

        char buf[80];
        if (i < (int)g_hosts.size()) {
            const auto& h = g_hosts[i];
            d.setCursor(6, y + 1);
            d.setTextColor(sel ? 0xFFE0 : 0xFFFF);
            d.print(h.name);
            d.setCursor(60, y + 1);
            d.setTextColor(sel ? 0xFFFF : 0x8C71);
            snprintf(buf, sizeof(buf), "%s@%s", h.user, h.host);
            d.print(buf);
        } else if (i < (int)(g_hosts.size() + g_savedHosts.size())) {
            const auto& h = g_savedHosts[i - g_hosts.size()];
            d.setCursor(6, y + 1);
            d.setTextColor(sel ? 0xFFE0 : 0x8C71);
            d.print("•");  // marker for NVS-stored
            d.setCursor(14, y + 1);
            d.setTextColor(sel ? 0xFFFF : 0xFFFF);
            snprintf(buf, sizeof(buf), "%s@%s", h.user, h.host);
            d.print(buf);
        } else {
            d.setCursor(6, y + 1);
            d.setTextColor(sel ? 0xFFE0 : 0x07FF);
            d.print("+ new host");
        }
        y += rowH;
    }

    if (!g_status.empty()) {
        d.setTextColor(0xF800);
        d.setCursor(6, 110);
        d.print(g_status.c_str());
    }

    drawFooter(";/ pick   enter connect   tab home");
    ui::flush();
}

void renderForm() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 3);
    d.print("new SSH host");

    int y = ui::statusbar::HEIGHT + 22;
    struct Row { const char* label; const std::string& v; FormField f; };
    Row rows[] = {
        {"host", g_formHost, FormField::Host},
        {"user", g_formUser, FormField::User},
    };
    for (auto& r : rows) {
        bool sel = (g_formField == r.f);
        if (sel) d.fillRoundRect(2, y - 1, SCREEN_W - 4, 14, 2, 0x18E3);
        d.setCursor(6, y + 1);
        d.setTextColor(sel ? 0xFFE0 : 0x8C71);
        d.print(r.label);
        d.setCursor(46, y + 1);
        d.setTextColor(0xFFFF);
        d.print(r.v.c_str());
        if (sel && ((millis() / 500) % 2 == 0)) d.print("_");
        y += 18;
    }

    drawFooter(";/  field   enter next/connect");
    ui::flush();
}

void renderAskSave() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0x07E0);
    d.setCursor(6, ui::statusbar::HEIGHT + 4);
    d.print("connected successfully");

    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 22);
    d.printf("%s@%s", g_active.user, g_active.host);

    d.setTextColor(0xFFE0);
    d.setCursor(6, ui::statusbar::HEIGHT + 50);
    d.print("save to NVS for quick reconnect?");

    drawFooter("y save   n skip   enter terminal");
    ui::flush();
}

void renderConnecting() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFE0);
    d.setCursor(6, 40);
    d.printf("connecting to %s…", g_active.name ? g_active.name : "?");
    d.setTextColor(0x8C71);
    d.setCursor(6, 56);
    d.printf("%s@%s",
             g_active.user ? g_active.user : "?",
             g_active.host ? g_active.host : "?");
    if (!g_status.empty()) {
        d.setTextColor(0xFFFF);
        d.setCursor(6, 80);
        d.print(g_status.c_str());
    }
    ui::flush();
}

void renderTerminal() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0x07E0);
    d.setCursor(6, ui::statusbar::HEIGHT + 3);
    if (g_active.user) d.printf("%s@%s", g_active.user, g_active.host);

    int topY    = ui::statusbar::HEIGHT + 16;
    int bottomY = SCREEN_H - 14 - 11;
    int lineH   = 10;
    int rows    = (bottomY - topY) / lineH;

    int start = (int)g_lines.size() - rows;
    if (start < 0) start = 0;
    int y = topY;
    for (int i = start; i < (int)g_lines.size(); i++) {
        d.setCursor(6, y);
        std::string s = g_lines[i];
        bool partial = (!s.empty() && s.back() == '\x01');
        if (partial) s.pop_back();
        d.setTextColor(partial ? 0xFFE0 : 0xFFFF);
        if (s.size() > 38) s = s.substr(s.size() - 38);
        d.print(s.c_str());
        y += lineH;
    }

    int inputY = SCREEN_H - 14 - 11;
    d.fillRect(0, inputY, SCREEN_W, 14, 0x2104);
    d.setTextColor(0xFFE0);
    d.setCursor(4, inputY + 3);
    d.print("$ ");
    d.setTextColor(0xFFFF);
    std::string in = g_input;
    if (in.size() > 36) in = std::string("…") + in.substr(in.size() - 35);
    d.print(in.c_str());
    if ((millis() / 500) % 2 == 0) d.print("_");

    drawFooter("enter send   tab home");
    ui::flush();
}

void renderError() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setTextSize(1);
    d.setTextColor(0xF800);
    d.setCursor(6, ui::statusbar::HEIGHT + 3);
    d.print("SSH error");
    d.setTextColor(0xFFFF);
    d.setCursor(6, 40);
    d.print(g_status.c_str());
    d.setTextColor(0x8C71);
    d.setCursor(6, 110);
    d.print("press enter for hosts");
    ui::flush();
}

void render() {
    switch (g_stage) {
        case Stage::Picker:     renderPicker();     break;
        case Stage::Form:       renderForm();       break;
        case Stage::Connecting: renderConnecting(); break;
        case Stage::AskSave:    renderAskSave();    break;
        case Stage::Connected:  renderTerminal();   break;
        case Stage::Error:      renderError();      break;
    }
}

// ----------- lifecycle -----------

void onEnter() {
    wifi::resume();    // SSH always needs the radio
    g_stage = Stage::Picker;
    g_status.clear();
    g_hosts = sealed::unsealSshHosts();
    loadSavedHosts();
    int total = (int)g_hosts.size() + (int)g_savedHosts.size() + 1;
    if (g_selected >= total) g_selected = 0;
    g_adhocConnected = false;
    g_dirty = true;
}
void onExit() { teardown(); g_stage = Stage::Picker; }

void onTick() {
    if (g_stage == Stage::Connected) pollChannel();

    if (g_stage == Stage::Connecting) {
        // First entry into Connecting: validate + kick off worker task.
        if (g_sshTask == nullptr) {
            if (!wifi::isConnected()) {
                g_status = "wifi not connected";
                g_stage  = Stage::Error;
                g_dirty  = true;
                return;
            }
            if (!g_active.host) {
                g_status = "no host";
                g_stage  = Stage::Error;
                g_dirty  = true;
                return;
            }
            if (!startConnection(g_active)) {
                teardown();
                g_stage = Stage::Error;
                g_dirty = true;
            }
            return;
        }
        // Worker running — wait for it to flip g_handshakeDone.
        if (!g_handshakeDone) return;
        g_sshTask = nullptr;
        bridge::resume();
        if (!g_handshakeOk) {
            teardown();
            g_stage = Stage::Error;
        } else if (g_adhocConnected) {
            g_stage = Stage::AskSave;
        } else {
            g_stage = Stage::Connected;
            pushLine("[connected]");
        }
        g_dirty = true;
    }

    static uint32_t lastBlink = 0;
    uint32_t now = millis();
    if (now - lastBlink >= 500) {
        lastBlink = now;
        g_dirty   = true;
    }
}

void onKey(char ch) {
    if (g_stage == Stage::Picker) {
        int sealedN = (int)g_hosts.size();
        int savedN  = (int)g_savedHosts.size();
        int total   = sealedN + savedN + 1;  // + "new"
        if (ch == key::Up) {
            g_selected = (g_selected - 1 + total) % total;
            g_dirty    = true;
        } else if (ch == key::Down) {
            g_selected = (g_selected + 1) % total;
            g_dirty    = true;
        } else if (ch == '\n') {
            if (g_selected < sealedN) {
                g_active = g_hosts[g_selected];
                g_adhocConnected = false;
            } else if (g_selected < sealedN + savedN) {
                g_active = g_savedHosts[g_selected - sealedN];
                g_adhocConnected = false;
            } else {
                // "+ new host"
                g_formHost.clear();
                g_formUser.clear();
                g_formField = FormField::Host;
                g_stage = Stage::Form;
                g_dirty = true;
                return;
            }
            g_lines.clear();
            g_status.clear();
            g_stage = Stage::Connecting;
            g_dirty = true;
        }
        return;
    }

    if (g_stage == Stage::Form) {
        std::string& field = (g_formField == FormField::Host) ? g_formHost : g_formUser;
        if (ch == key::Up || ch == key::Down) {
            g_formField = (g_formField == FormField::Host) ? FormField::User : FormField::Host;
        } else if (ch == '\n') {
            if (g_formField == FormField::Host) {
                g_formField = FormField::User;
            } else {
                if (g_formHost.empty() || g_formUser.empty()) {
                    g_status = "host + user required";
                    g_dirty  = true;
                    return;
                }
                // Stash as a transient SshHost; pointers live in g_form*.
                g_active.name = g_formHost.c_str();
                g_active.host = g_formHost.c_str();
                g_active.user = g_formUser.c_str();
                g_active.port = 22;
                g_adhocConnected = true;
                g_lines.clear();
                g_status.clear();
                g_stage = Stage::Connecting;
            }
        } else if (ch == '\b') {
            if (!field.empty()) field.pop_back();
        } else if (ch >= 0x20 && ch <= 0x7E) {
            field.push_back(ch);
        }
        g_dirty = true;
        return;
    }

    if (g_stage == Stage::AskSave) {
        if (ch == 'y' || ch == 'Y') {
            SshHost h;
            h.host = g_formHost.c_str();
            h.user = g_formUser.c_str();
            h.port = 22;
            saveHost(h);
            // Refresh saved list so a later picker shows the new entry.
            // (g_active currently points into g_form* which stay valid.)
            g_stage = Stage::Connected;
            pushLine("[connected, saved to NVS]");
        } else if (ch == 'n' || ch == 'N' || ch == '\n') {
            g_stage = Stage::Connected;
            pushLine("[connected]");
        }
        g_dirty = true;
        return;
    }

    if (g_stage == Stage::Connected) {
        if (ch == '\n')      sendCommand();
        else if (ch == '\b') { if (!g_input.empty()) { g_input.pop_back(); g_dirty = true; } }
        else if (ch >= 0x20 && ch <= 0x7E) { g_input.push_back(ch); g_dirty = true; }
        return;
    }

    if (g_stage == Stage::Error) {
        if (ch == '\n') {
            g_status.clear();
            g_stage = Stage::Picker;
            g_dirty = true;
        }
    }
}

void onDraw() { if (g_dirty) { render(); g_dirty = false; } }

App ssh_app = {
    .id           = "ssh",
    .name         = "SSH",
    .description  = "Key-auth SSH client",
    .services     = SVC_WIFI,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .onDraw       = onDraw,
    .onEvent      = nullptr,
    .keysAsArrows = true,  // picker is a menu; terminal is text-only and uses different stage
};

}  // namespace

REGISTER_APP(ssh_app);
