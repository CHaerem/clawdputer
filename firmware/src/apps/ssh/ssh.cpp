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
#include <libssh/libssh.h>
#include <libssh_esp32.h>

#include <deque>
#include <string>

#include "core/app.h"
#include "core/key.h"
#include "secrets/ssh_hosts.h"
#include "services/identity.h"
#include "services/sealed.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

enum class Stage : uint8_t {
    Picker,        // choose a host from kSshHosts
    Connecting,
    Connected,
    Error,
};

Stage                g_stage    = Stage::Picker;
int                  g_selected = 0;
std::string          g_status;
bool                 g_dirty    = true;
std::vector<SshHost> g_hosts;

std::deque<std::string> g_lines;
std::string             g_input;
ssh_session             g_session = nullptr;
ssh_channel             g_channel = nullptr;
SshHost                 g_active  = {};

constexpr size_t MAX_LINES = 100;

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
}

bool startConnection(const SshHost& h) {
    g_session = ssh_new();
    if (!g_session) { g_status = "ssh_new failed"; return false; }

    int port    = h.port > 0 ? h.port : 22;
    int verbose = SSH_LOG_NOLOG;
    ssh_options_set(g_session, SSH_OPTIONS_HOST,    h.host);
    ssh_options_set(g_session, SSH_OPTIONS_USER,    h.user);
    ssh_options_set(g_session, SSH_OPTIONS_PORT,    &port);
    ssh_options_set(g_session, SSH_OPTIONS_LOG_VERBOSITY, &verbose);

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
    if (ssh_channel_request_shell(g_channel) != SSH_OK) {
        g_status = std::string("shell: ") + ssh_get_error(g_session);
        return false;
    }
    ssh_channel_set_blocking(g_channel, 0);
    return true;
}

void pollChannel() {
    if (!g_channel) return;

    char buf[256];
    int  n;
    static std::string carry;

    while ((n = ssh_channel_read_nonblocking(g_channel, buf, sizeof(buf) - 1, 0)) > 0) {
        carry.append(buf, n);
        size_t start = 0;
        for (size_t i = 0; i < carry.size(); i++) {
            if (carry[i] == '\n') {
                std::string line;
                for (size_t j = start; j < i; j++) {
                    char c = carry[j];
                    if (c == '\r') continue;
                    if (c >= 0x20 && c <= 0x7E) line.push_back(c);
                }
                pushLine(line);
                start = i + 1;
            }
        }
        carry.erase(0, start);
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

    if (g_hosts.empty()) {
        d.setTextColor(0xF800);
        d.setCursor(6, 40);
        d.print("no sealed hosts");
        d.setTextColor(0x8C71);
        d.setCursor(6, 56);
        d.print("seal hosts via tools/seal-hosts.py,");
        d.setCursor(6, 68);
        d.print("commit, push, then GitOps deploys.");
        drawFooter("tab home");
        ui::flush();
        return;
    }

    int y = ui::statusbar::HEIGHT + 18;
    for (size_t i = 0; i < g_hosts.size(); i++) {
        bool sel = (int)i == g_selected;
        if (sel) d.fillRoundRect(2, y - 1, SCREEN_W - 4, 16, 2, 0x18E3);

        d.setCursor(6, y + 1);
        d.setTextColor(sel ? 0xFFE0 : 0xFFFF);
        d.print(g_hosts[i].name);

        d.setCursor(60, y + 1);
        d.setTextColor(sel ? 0xFFFF : 0x8C71);
        char buf[80];
        snprintf(buf, sizeof(buf), "%s@%s", g_hosts[i].user, g_hosts[i].host);
        d.print(buf);

        y += 18;
        if (y > 110) break;
    }

    if (!g_status.empty()) {
        d.setTextColor(0xF800);
        d.setCursor(6, 110);
        d.print(g_status.c_str());
    }

    drawFooter(";/ pick   enter connect   tab home");
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
        d.setTextColor(0xFFFF);
        d.setCursor(6, y);
        std::string s = g_lines[i];
        if (s.size() > 38) s = s.substr(0, 37) + "…";
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
        case Stage::Connecting: renderConnecting(); break;
        case Stage::Connected:  renderTerminal();   break;
        case Stage::Error:      renderError();      break;
    }
}

// ----------- lifecycle -----------

void onEnter() {
    g_stage = Stage::Picker;
    g_status.clear();
    g_hosts = sealed::unsealSshHosts();
    if (g_selected >= (int)g_hosts.size()) g_selected = 0;
    g_dirty = true;
}
void onExit() { teardown(); g_stage = Stage::Picker; }

void onTick() {
    if (g_stage == Stage::Connected) pollChannel();

    if (g_stage == Stage::Connecting) {
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
        int n = (int)g_hosts.size();
        if (ch == key::Up && n > 0) {
            g_selected = (g_selected - 1 + n) % n;
            g_dirty    = true;
        } else if (ch == key::Down && n > 0) {
            g_selected = (g_selected + 1) % n;
            g_dirty    = true;
        } else if (ch == '\n' && n > 0) {
            g_active = g_hosts[g_selected];
            g_lines.clear();
            g_status.clear();
            g_stage  = Stage::Connecting;
            g_dirty  = true;
        }
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
