// Game Boy emulator app — loads .gb ROMs from microSD and runs them via Peanut-GB.
//
// Stages:
//   Picker      file browser of .gb files on SD root and /roms/, plus a
//               synthetic "[+ Download ROMs]" row that opens the downloader
//   Sources     list of manifest URLs read from /roms/sources.txt
//   Manifest    list of ROM URLs fetched from the selected source
//   Playing     emulation loop; Tab exits to home (caught by main.cpp)
//
// On-device downloader:
//   /roms/sources.txt   one manifest URL per line. '#' starts a comment.
//   each manifest       one ROM URL per line. Optional "Display name | URL".
//   Downloaded files are written to /roms/<basename> and immediately appear
//   in the picker on next entry.
//
// Controls during play:
//   E/S/A/D = Up/Down/Left/Right   K = B   L = A   1/Enter = Start   2 = Select

#include <Arduino.h>
#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFiClientSecure.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/app.h"
#include "services/sd.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/list.h"
#include "ui/statusbar.h"
#include "ui/toast.h"

#define ENABLE_LCD 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#include "peanut_gb.h"

namespace {

// Maximum ROM size we'll load into heap. Larger ROMs require PSRAM (not present
// on ESP32-S3FN8). 256 KB covers Tetris, Super Mario Land, Kirby's Dream Land, etc.
constexpr size_t MAX_ROM_BYTES = 256 * 1024;

// Display layout: GB native is 160×144. Cardputer is 240×135.
// We keep the full 160px width (centred with 40px margins) and scale 144→135 rows
// using nearest-neighbour (same approach as gb_cardputer reference port).
constexpr int GB_W      = 160;
constexpr int GB_H      = 144;
constexpr int DEST_H    = 135;   // SCREEN_H
constexpr int X_OFFSET  = (SCREEN_W - GB_W) / 2;   // 40

// DMG greyscale palette (RGB565): white, light grey, dark grey, black
constexpr uint16_t PALETTE[4] = { 0xFFFF, 0xAD55, 0x52AA, 0x0000 };

constexpr const char* SOURCES_PATH = "/roms/sources.txt";
constexpr const char* ROMS_DIR     = "/roms";

enum class Stage { Picker, Sources, Manifest, Playing };

Stage           g_stage = Stage::Picker;
ui::list::State g_picker;
ui::list::State g_sources;     // items: label=display, value=manifest URL
ui::list::State g_manifest;    // items: label=display, value=rom URL

uint8_t*  g_rom     = nullptr;
uint8_t*  g_cartRam = nullptr;
struct gb_s g_gb;

uint16_t g_fb[GB_H][GB_W]     = {};
uint16_t g_prevFb[DEST_H][GB_W] = {};

uint32_t g_lastFrameMs = 0;

// ── Peanut-GB callbacks ───────────────────────────────────────────────────────

uint8_t gb_rom_read(struct gb_s*, uint_fast32_t addr) {
    return g_rom[addr];
}
uint8_t gb_cart_ram_read(struct gb_s*, uint_fast32_t addr) {
    return g_cartRam ? g_cartRam[addr] : 0xFF;
}
void gb_cart_ram_write(struct gb_s*, uint_fast32_t addr, uint8_t val) {
    if (g_cartRam) g_cartRam[addr] = val;
}
void gb_error(struct gb_s*, const enum gb_error_e err, const uint16_t addr) {
    Serial.printf("[gb] error %d at 0x%04X\n", (int)err, addr);
}
void lcd_draw_line(struct gb_s*, const uint8_t *pixels, const uint_fast8_t line) {
    for (int x = 0; x < GB_W; x++) g_fb[line][x] = PALETTE[pixels[x] & 3];
}

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

// "Display | URL"  →  { "Display", "URL" }.   "URL" alone → { basename, URL }.
void parseManifestLine(const std::string& raw, std::string& nameOut, std::string& urlOut) {
    std::string line = trim(raw);
    auto bar = line.find('|');
    if (bar != std::string::npos) {
        nameOut = trim(line.substr(0, bar));
        urlOut  = trim(line.substr(bar + 1));
        return;
    }
    urlOut = line;
    auto slash = line.find_last_of('/');
    nameOut = (slash == std::string::npos) ? line : line.substr(slash + 1);
    auto q = nameOut.find('?');
    if (q != std::string::npos) nameOut = nameOut.substr(0, q);
}

bool isHtmlContent(const std::string& content) {
    std::string lower = content;
    for (auto& c : lower) c = tolower(c);
    return lower.find("<!doctype") != std::string::npos ||
           lower.find("<html") != std::string::npos ||
           lower.find("<body") != std::string::npos ||
           lower.find("</a>") != std::string::npos;
}

std::string extractHrefValue(const std::string& tag) {
    auto hrefPos = tag.find("href");
    if (hrefPos == std::string::npos) return "";
    size_t eqPos = tag.find('=', hrefPos);
    if (eqPos == std::string::npos || eqPos >= tag.size()) return "";
    size_t startPos = eqPos + 1;
    while (startPos < tag.size() && (tag[startPos] == ' ' || tag[startPos] == '\t')) startPos++;
    if (startPos >= tag.size()) return "";
    char quote = tag[startPos];
    if (quote == '"' || quote == '\'') {
        startPos++;
        size_t endPos = tag.find(quote, startPos);
        if (endPos == std::string::npos) return "";
        return tag.substr(startPos, endPos - startPos);
    }
    size_t endPos = startPos;
    while (endPos < tag.size() && tag[endPos] != ' ' && tag[endPos] != '>') endPos++;
    return tag.substr(startPos, endPos - startPos);
}

bool extractGbLinksFromHtml(const std::string& html, const std::string& baseUrl) {
    g_manifest.items.clear();
    g_manifest.selected  = 0;
    g_manifest.scrollTop = 0;

    std::string baseDir = baseUrl;
    size_t lastSlash = baseDir.find_last_of('/');
    baseDir = (lastSlash != std::string::npos) ? baseDir.substr(0, lastSlash + 1) : baseUrl + "/";

    size_t pos = 0;
    while ((pos = html.find("<a", pos)) != std::string::npos) {
        size_t tagEnd = html.find(">", pos);
        if (tagEnd == std::string::npos) { pos++; continue; }
        std::string tag  = html.substr(pos, tagEnd - pos);
        std::string href = extractHrefValue(tag);
        if (href.empty() || href.find(".gb") == std::string::npos) { pos = tagEnd + 1; continue; }

        std::string fullUrl = href;
        if (href[0] == '/') {
            size_t protoEnd = baseUrl.find("://");
            if (protoEnd != std::string::npos) {
                size_t hostEnd = baseUrl.find('/', protoEnd + 3);
                if (hostEnd != std::string::npos) fullUrl = baseUrl.substr(0, hostEnd) + href;
            }
        } else if (href.find("://") == std::string::npos && href[0] != '#') {
            fullUrl = baseDir + href;
        }

        size_t ls = fullUrl.find_last_of('/');
        std::string displayName = (ls != std::string::npos) ? fullUrl.substr(ls + 1) : fullUrl;
        size_t qp = displayName.find('?');
        if (qp != std::string::npos) displayName = displayName.substr(0, qp);

        bool dup = false;
        for (const auto& item : g_manifest.items) { if (item.value == fullUrl) { dup = true; break; } }
        if (!dup && !displayName.empty()) g_manifest.items.push_back({displayName, fullUrl});
        pos = tagEnd + 1;
    }
    return !g_manifest.items.empty();
}

void buildFileList() {
    g_picker.items.clear();
    g_picker.selected  = 0;
    g_picker.scrollTop = 0;

    g_picker.items.push_back({"[+ Download ROMs]", "", true});

    auto addDir = [](const char* path) {
        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) return;
        File f;
        while ((f = dir.openNextFile())) {
            std::string name = f.name();
            if (!f.isDirectory() && name.size() > 3 &&
                name.substr(name.size() - 3) == ".gb") {
                std::string full = std::string(path) + "/" + name;
                if (full.size() > 1 && full[0] == '/' && full[1] == '/')
                    full = full.substr(1);
                g_picker.items.push_back({name, full});
            }
            f.close();
        }
    };

    addDir("/");
    addDir(ROMS_DIR);
}

void buildSourcesList() {
    g_sources.items.clear();
    g_sources.selected  = 0;
    g_sources.scrollTop = 0;

    File f = SD.open(SOURCES_PATH);
    if (f) {
        while (f.available()) {
            std::string line = trim(std::string(f.readStringUntil('\n').c_str()));
            if (line.empty() || line[0] == '#') continue;
            std::string name, url;
            parseManifestLine(line, name, url);
            if (url.empty()) continue;
            g_sources.items.push_back({name, url});
        }
        f.close();
    } else {
        // No sources.txt on SD — use built-in default
        g_sources.items.push_back({"ROMs Games (HTML)", "https://www.romsgames.net/roms/gameboy/"});
    }
}

void drawProgress(const char* title, const char* sub, int pct) {
    auto& d = ui::display();
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(8, 8);
    d.print(title);
    d.setTextSize(1);
    d.setTextColor(0x7BEF);
    d.setCursor(8, 36);
    d.print(sub);
    int barX = 8, barY = 60, barW = SCREEN_W - 16, barH = 10;
    d.drawRect(barX, barY, barW, barH, WHITE);
    if (pct >= 0) {
        int fill = (barW - 2) * pct / 100;
        d.fillRect(barX + 1, barY + 1, fill, barH - 2, 0x07E0);
    }
}

bool httpGet(const std::string& url, std::string& bodyOut, std::string& errOut) {
    if (!wifi::isConnected()) { errOut = "wifi offline"; return false; }
    bool secure = url.rfind("https://", 0) == 0;
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    bool ok;
    WiFiClientSecure tls;
    WiFiClient       plain;
    if (secure) { tls.setInsecure(); ok = http.begin(tls, url.c_str()); }
    else        { ok = http.begin(plain, url.c_str()); }
    if (!ok) { errOut = "http begin failed"; return false; }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[48]; snprintf(buf, sizeof(buf), "http %d", code);
        errOut = buf; http.end(); return false;
    }
    bodyOut = http.getString().c_str();
    http.end();
    return true;
}

bool fetchManifest(const std::string& url) {
    drawProgress("Loading", url.c_str(), -1);

    std::string body, err;
    if (!httpGet(url, body, err)) {
        ui::toast::show(err);
        return false;
    }

    if (isHtmlContent(body)) {
        bool ok = extractGbLinksFromHtml(body, url);
        if (!ok) { ui::toast::show("No .gb files found in HTML"); return false; }
        return true;
    }

    g_manifest.items.clear();
    g_manifest.selected  = 0;
    g_manifest.scrollTop = 0;

    size_t start = 0;
    while (start <= body.size()) {
        size_t nl = body.find('\n', start);
        std::string line = trim(body.substr(start, (nl == std::string::npos ? body.size() : nl) - start));
        if (!line.empty() && line[0] != '#') {
            std::string name, romUrl;
            parseManifestLine(line, name, romUrl);
            if (!romUrl.empty()) g_manifest.items.push_back({name, romUrl});
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (g_manifest.items.empty()) {
        ui::toast::show("Manifest is empty");
        return false;
    }
    return true;
}

bool downloadRom(const std::string& name, const std::string& url) {
    if (!wifi::isConnected()) { ui::toast::show("WiFi offline"); return false; }

    SD.mkdir(ROMS_DIR);
    std::string fname = name;
    auto slash = fname.find_last_of('/');
    if (slash != std::string::npos) fname = fname.substr(slash + 1);
    if (fname.size() < 3 || fname.substr(fname.size() - 3) != ".gb") fname += ".gb";
    std::string path = std::string(ROMS_DIR) + "/" + fname;

    drawProgress("Downloading", fname.c_str(), 0);

    bool secure = url.rfind("https://", 0) == 0;
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    bool ok;
    WiFiClientSecure tls;
    WiFiClient       plain;
    if (secure) { tls.setInsecure(); ok = http.begin(tls, url.c_str()); }
    else        { ok = http.begin(plain, url.c_str()); }
    if (!ok) { ui::toast::show("http begin failed"); return false; }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[48]; snprintf(buf, sizeof(buf), "http %d", code);
        ui::toast::show(buf); http.end(); return false;
    }

    int total = http.getSize();
    if (total > 0 && (size_t)total > MAX_ROM_BYTES) {
        ui::toast::show("ROM too large (>256 KB)");
        http.end(); return false;
    }

    File out = SD.open(path.c_str(), FILE_WRITE);
    if (!out) { ui::toast::show("SD open failed"); http.end(); return false; }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    int got = 0;
    int lastPct = -1;
    uint32_t deadline = millis() + 60000;
    while (http.connected() && (total <= 0 || got < total)) {
        size_t avail = stream->available();
        if (avail) {
            int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
            if (n <= 0) break;
            if ((size_t)(got + n) > MAX_ROM_BYTES) {
                ui::toast::show("ROM too large (>256 KB)");
                out.close(); SD.remove(path.c_str()); http.end(); return false;
            }
            out.write(buf, n);
            got += n;
            if (total > 0) {
                int pct = (int)((int64_t)got * 100 / total);
                if (pct != lastPct) { drawProgress("Downloading", fname.c_str(), pct); lastPct = pct; }
            }
            deadline = millis() + 60000;
        } else if (millis() > deadline) {
            ui::toast::show("Download timeout");
            out.close(); SD.remove(path.c_str()); http.end(); return false;
        } else {
            delay(1);
        }
    }
    out.close();
    http.end();

    if (got == 0) { SD.remove(path.c_str()); ui::toast::show("Empty download"); return false; }
    ui::toast::show("Saved to /roms");
    return true;
}

bool launchRom(const std::string& path) {
    File f = SD.open(path.c_str());
    if (!f) {
        ui::toast::show("Cannot open file");
        return false;
    }
    size_t size = f.size();
    if (size > MAX_ROM_BYTES) {
        f.close();
        ui::toast::show("ROM too large (>256 KB)");
        return false;
    }

    free(g_rom);
    g_rom = (uint8_t*)malloc(size);
    if (!g_rom) {
        f.close();
        ui::toast::show("Out of memory");
        return false;
    }
    f.readBytes((char*)g_rom, size);
    f.close();

    enum gb_init_error_e err = gb_init(&g_gb, gb_rom_read, gb_cart_ram_read,
                                        gb_cart_ram_write, gb_error, nullptr);
    if (err != GB_INIT_NO_ERROR) {
        free(g_rom); g_rom = nullptr;
        ui::toast::show("ROM init failed");
        return false;
    }

    gb_init_lcd(&g_gb, lcd_draw_line);

    free(g_cartRam);
    size_t saveSize = 0;
    gb_get_save_size_s(&g_gb, &saveSize);
    g_cartRam = saveSize ? (uint8_t*)calloc(1, saveSize) : nullptr;

    memset(g_fb,     0, sizeof(g_fb));
    memset(g_prevFb, 0, sizeof(g_prevFb));
    g_lastFrameMs = millis();
    return true;
}

void renderFrame() {
    auto& d = ui::display();
    for (int dy = 0; dy < DEST_H; dy++) {
        int  srcRow = dy * GB_H / DEST_H;
        for (int x = 0; x < GB_W; x++) {
            uint16_t px = g_fb[srcRow][x];
            if (px != g_prevFb[dy][x]) {
                d.drawPixel(X_OFFSET + x, dy, px);
                g_prevFb[dy][x] = px;
            }
        }
    }
    ui::flush();
}

void drawListStage(const char* title, ui::list::State& list, const char* emptyMsg) {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(4, ui::statusbar::HEIGHT + 2);
    d.print(title);

    if (list.items.empty()) {
        d.setTextColor(0x7BEF);
        d.setCursor(8, SCREEN_H / 2 - 4);
        d.print(emptyMsg);
    } else {
        ui::list::draw(list, 0, ui::statusbar::HEIGHT + 12,
                       SCREEN_W, SCREEN_H - ui::statusbar::HEIGHT - 12);
    }

    ui::toast::draw();
    ui::flush();
}

// ── App lifecycle ─────────────────────────────────────────────────────────────

void onEnter() {
    if (g_stage == Stage::Picker) {
        buildFileList();
    }
}

void onExit() {
    free(g_rom);    g_rom    = nullptr;
    free(g_cartRam); g_cartRam = nullptr;
    g_stage = Stage::Picker;
}

void onTick() {
    switch (g_stage) {
        case Stage::Picker:
            drawListStage("Select ROM", g_picker, "No .gb files on SD card");
            return;
        case Stage::Sources:
            drawListStage("Sources", g_sources,
                          "(built-ins shown; add /roms/sources.txt to customize)");
            return;
        case Stage::Manifest:
            drawListStage("Pick ROM to download", g_manifest, "(empty)");
            return;
        case Stage::Playing:
            break;
    }

    uint32_t now = millis();
    if (now - g_lastFrameMs < 16) return;
    g_lastFrameMs = now;

    g_gb.direct.joypad = 0xFF;
    if (M5Cardputer.Keyboard.isPressed()) {
        auto ks = M5Cardputer.Keyboard.keysState();
        for (char c : ks.word) {
            switch (c) {
                case 'e': g_gb.direct.joypad_bits.up     = 0; break;
                case 's': g_gb.direct.joypad_bits.down   = 0; break;
                case 'a': g_gb.direct.joypad_bits.left   = 0; break;
                case 'd': g_gb.direct.joypad_bits.right  = 0; break;
                case 'k': g_gb.direct.joypad_bits.b      = 0; break;
                case 'l': g_gb.direct.joypad_bits.a      = 0; break;
                case '1': g_gb.direct.joypad_bits.start  = 0; break;
                case '2': g_gb.direct.joypad_bits.select = 0; break;
            }
        }
        if (ks.enter) g_gb.direct.joypad_bits.start  = 0;
        if (ks.space) g_gb.direct.joypad_bits.select = 0;
    }

    gb_run_frame(&g_gb);
    renderFrame();
}

void onKey(char ch) {
    if (g_stage == Stage::Picker) {
        if (ch == '\n' && !g_picker.items.empty()) {
            const auto& item = g_picker.items[g_picker.selected];
            if (item.action) {
                buildSourcesList();
                g_stage = Stage::Sources;
            } else if (launchRom(item.value)) {
                g_stage = Stage::Playing;
            }
            return;
        }
        ui::list::onKey(g_picker, ch);
        return;
    }

    if (g_stage == Stage::Sources) {
        if (ch == key::Left || ch == '\b') { g_stage = Stage::Picker; return; }
        if (ch == '\n' && !g_sources.items.empty()) {
            const auto& src = g_sources.items[g_sources.selected];
            if (fetchManifest(src.value)) g_stage = Stage::Manifest;
            return;
        }
        ui::list::onKey(g_sources, ch);
        return;
    }

    if (g_stage == Stage::Manifest) {
        if (ch == key::Left || ch == '\b') { g_stage = Stage::Sources; return; }
        if (ch == '\n' && !g_manifest.items.empty()) {
            const auto& rom = g_manifest.items[g_manifest.selected];
            if (downloadRom(rom.label, rom.value)) {
                buildFileList();
                g_stage = Stage::Picker;
            }
            return;
        }
        ui::list::onKey(g_manifest, ch);
        return;
    }
}

static App gameboy_app = {
    .id           = "gameboy",
    .name         = "Game Boy",
    .description  = "GB emulator — ROMs on SD card",
    .services     = SVC_SD | SVC_WIFI | SVC_CANVAS,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .keysAsArrows = false,
};
REGISTER_APP(gameboy_app);

}  // namespace
