// SSH client — native libssh over WiFi.
//
// MVP scope: password auth, plain shell (no PTY/ANSI), single connection
// at a time. Host/user/password collected on a form screen, then a simple
// terminal view streams output and sends typed commands.
//
// Known-hosts handling: trust-on-first-use. We don't have NVS-backed known
// hosts yet — a connection just accepts whatever public key the server
// presents. Acceptable for a developer-only device on trusted networks;
// must be revisited before this is used against untrusted networks.

#include <Arduino.h>
#include <M5Cardputer.h>
#include <libssh/libssh.h>
#include <libssh_esp32.h>

#include <deque>
#include <string>

#include "core/app.h"
#include "core/key.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

enum class Stage : uint8_t {
    Form,          // user is filling in host/user/pass
    Connecting,    // ssh handshake in progress
    Connected,     // shell channel open
    Error,
};

enum class Field : uint8_t { Host, User, Pass };

Stage       g_stage = Stage::Form;
Field       g_field = Field::Host;
std::string g_host, g_user, g_pass;
std::string g_status;
bool        g_dirty = true;

std::deque<std::string> g_lines;
std::string             g_input;
ssh_session             g_session = nullptr;
ssh_channel             g_channel = nullptr;

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
}

bool startConnection() {
    g_session = ssh_new();
    if (!g_session) { g_status = "ssh_new failed"; return false; }

    int port    = 22;
    int verbose = SSH_LOG_NOLOG;
    ssh_options_set(g_session, SSH_OPTIONS_HOST,    g_host.c_str());
    ssh_options_set(g_session, SSH_OPTIONS_USER,    g_user.c_str());
    ssh_options_set(g_session, SSH_OPTIONS_PORT,    &port);
    ssh_options_set(g_session, SSH_OPTIONS_LOG_VERBOSITY, &verbose);

    int rc = ssh_connect(g_session);
    if (rc != SSH_OK) {
        g_status = std::string("connect: ") + ssh_get_error(g_session);
        return false;
    }

    // Trust-on-first-use. No NVS-backed known-hosts yet.
    rc = ssh_userauth_password(g_session, nullptr, g_pass.c_str());
    if (rc != SSH_AUTH_SUCCESS) {
        g_status = std::string("auth: ") + ssh_get_error(g_session);
        return false;
    }

    g_channel = ssh_channel_new(g_session);
    if (!g_channel) { g_status = "channel_new failed"; return false; }

    rc = ssh_channel_open_session(g_channel);
    if (rc != SSH_OK) {
        g_status = std::string("open: ") + ssh_get_error(g_session);
        return false;
    }
    rc = ssh_channel_request_shell(g_channel);
    if (rc != SSH_OK) {
        g_status = std::string("shell: ") + ssh_get_error(g_session);
        return false;
    }

    ssh_channel_set_blocking(g_channel, 0);
    return true;
}

void pollChannel() {
    if (!g_channel) return;

    char    buf[256];
    int     n;
    static std::string carry;

    while ((n = ssh_channel_read_nonblocking(g_channel, buf, sizeof(buf) - 1, 0)) > 0) {
        carry.append(buf, n);
        // Split into lines on '\n', strip CR. Drop bytes outside printable
        // ASCII for now (no ANSI handling yet — we'll surface them later).
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

void drawHeader(const char* title, uint16_t color) {
    auto& d = ui::display();
    d.setTextSize(1);
    d.setTextColor(color);
    d.setCursor(6, ui::statusbar::HEIGHT + 3);
    d.print(title);
}

void renderForm() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    drawHeader("SSH connect", 0xFFFF);

    int y = ui::statusbar::HEIGHT + 18;
    struct Row { const char* label; const std::string& value; bool isPass; Field f; };
    Row rows[] = {
        {"host", g_host, false, Field::Host},
        {"user", g_user, false, Field::User},
        {"pass", g_pass, true,  Field::Pass},
    };

    for (auto& r : rows) {
        bool sel = (g_field == r.f);
        if (sel) d.fillRoundRect(2, y - 1, SCREEN_W - 4, 14, 2, 0x18E3);

        d.setCursor(6, y + 2);
        d.setTextColor(sel ? 0xFFE0 : 0x8C71);
        d.print(r.label);

        d.setCursor(40, y + 2);
        d.setTextColor(0xFFFF);
        if (r.isPass) {
            std::string masked(r.value.size(), '*');
            d.print(masked.c_str());
        } else {
            d.print(r.value.c_str());
        }
        if (sel && ((millis() / 500) % 2 == 0)) d.print("_");
        y += 18;
    }

    if (!g_status.empty()) {
        d.setCursor(6, y + 4);
        d.setTextColor(0xF800);
        d.print(g_status.c_str());
    }

    // Footer
    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print(";/ field   enter next/connect   tab home");

    ui::flush();
}

void renderConnecting() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    drawHeader("SSH", 0xFFE0);

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, 50);
    d.printf("connecting to %s…", g_host.c_str());
    if (!g_status.empty()) {
        d.setTextColor(0x8C71);
        d.setCursor(6, 64);
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
    d.printf("%s@%s", g_user.c_str(), g_host.c_str());

    // Output viewport
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

    // Input strip
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

    // Footer
    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print("enter send   tab home (disconnects)");

    ui::flush();
}

void renderError() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    drawHeader("SSH error", 0xF800);

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, 40);
    d.print(g_status.c_str());

    d.setTextColor(0x8C71);
    d.setCursor(6, 110);
    d.print("press enter to retry");
    ui::flush();
}

void render() {
    switch (g_stage) {
        case Stage::Form:       renderForm();       break;
        case Stage::Connecting: renderConnecting(); break;
        case Stage::Connected:  renderTerminal();   break;
        case Stage::Error:      renderError();      break;
    }
}

// ----------- lifecycle -----------

void onEnter() {
    g_stage  = Stage::Form;
    g_field  = Field::Host;
    g_status.clear();
    g_dirty  = true;
}

void onExit() { teardown(); g_stage = Stage::Form; }

void onTick() {
    if (g_stage == Stage::Connected) {
        pollChannel();
    } else if (g_stage == Stage::Connecting) {
        // Run the (blocking) connect on the main loop. Cardputer's idle is
        // generous and the user is already staring at a "connecting…" screen.
        if (!wifi::isConnected()) {
            g_status = "wifi not connected";
            g_stage  = Stage::Error;
            g_dirty  = true;
            return;
        }
        bool ok = startConnection();
        if (!ok) {
            teardown();
            g_stage = Stage::Error;
        } else {
            g_stage = Stage::Connected;
            pushLine("[connected]");
        }
        g_dirty = true;
    }
    // Cursor blink
    static uint32_t lastBlink = 0;
    uint32_t now = millis();
    if (now - lastBlink >= 500) {
        lastBlink = now;
        g_dirty   = true;
    }
}

std::string& currentField() {
    switch (g_field) {
        case Field::Host: return g_host;
        case Field::User: return g_user;
        case Field::Pass: return g_pass;
    }
    return g_host;
}

void onKey(char ch) {
    if (g_stage == Stage::Form) {
        if (ch == key::Up) {
            g_field = (g_field == Field::Host) ? Field::Pass
                    : (g_field == Field::User) ? Field::Host
                    :                            Field::User;
            g_dirty = true;
        } else if (ch == key::Down) {
            g_field = (g_field == Field::Host) ? Field::User
                    : (g_field == Field::User) ? Field::Pass
                    :                            Field::Host;
            g_dirty = true;
        } else if (ch == '\n') {
            if (g_field == Field::Pass) {
                if (g_host.empty() || g_user.empty()) {
                    g_status = "host and user required";
                    g_dirty = true;
                    return;
                }
                g_status = "connecting…";
                g_stage  = Stage::Connecting;
                g_dirty  = true;
            } else {
                g_field = (g_field == Field::Host) ? Field::User : Field::Pass;
                g_dirty = true;
            }
        } else if (ch == '\b') {
            auto& f = currentField();
            if (!f.empty()) { f.pop_back(); g_dirty = true; }
        } else if (ch >= 0x20 && ch <= 0x7E) {
            currentField().push_back(ch);
            g_dirty = true;
        }
        return;
    }

    if (g_stage == Stage::Connected) {
        if (ch == '\n') {
            sendCommand();
        } else if (ch == '\b') {
            if (!g_input.empty()) { g_input.pop_back(); g_dirty = true; }
        } else if (ch >= 0x20 && ch <= 0x7E) {
            g_input.push_back(ch);
            g_dirty = true;
        }
        return;
    }

    if (g_stage == Stage::Error) {
        if (ch == '\n') {
            g_status.clear();
            g_stage = Stage::Form;
            g_dirty = true;
        }
    }
}

void onDraw() {
    if (!g_dirty) return;
    render();
    g_dirty = false;
}

App ssh_app = {
    .id           = "ssh",
    .name         = "SSH",
    .description  = "WiFi SSH client",
    .services     = SVC_WIFI,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .onDraw       = onDraw,
    .onEvent      = nullptr,
    .keysAsArrows = false,  // text-input app
};

}  // namespace

REGISTER_APP(ssh_app);
