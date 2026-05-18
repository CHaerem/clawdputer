// Retro game console emulator app — single home for all vendored cores.
// Currently ships only GnuBoy (GB / GBC). Future cores (NES via Nofrendo,
// SMS/GG via SMSPlus, NGP via Race, …) plug in here as siblings: add an
// entry to kSupportedExts, a Core enum value, and a case in dispatch().
//
// ROMs live under /roms/ on the SD card. The picker scans for any
// supported extension and shows a flat list prefixed with the system label.
// A "[+ Download ROMs]" row at the top opens an on-device HTTPS downloader.
// Saves are written to /saves/<basename>.sav.
//
// On-device downloader:
//   /roms/sources.txt   one manifest URL per line (# starts a comment).
//                        Each manifest URL is either a plain-text list of
//                        ROM URLs (one per line, optional "Display name | URL")
//                        or an HTML index page that we scrape for any
//                        supported-extension hrefs.
//
// Per-core notes:
//   GnuBoy (GPLv2, firmware/lib/gnuboy/) — no sound in v1; pending a
//   streaming-PCM service. ROM size cap 256 KB until we add flash-mmap
//   loading.
//
// Display: GB frame is 160×144, our LCD is 240×135. Letterbox to x=40,
// crop 5 rows from the top + 4 from the bottom (rows usually carry less
// critical content than the playfield).
//
// Controls during play:
//   E/S/A/D = Up/Down/Left/Right   K = B   L = A   1 = Start   2 = Select
//   Tab returns to the launcher.

#include <Arduino.h>
#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "services/ble.h"
#include "services/sd.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/list.h"
#include "ui/statusbar.h"
#include "ui/toast.h"

extern "C" {
#include "gnuboy.h"
}

namespace {

constexpr const char* ROMS_DIR     = "/roms";
constexpr const char* SAVES_DIR    = "/saves";
constexpr const char* SOURCES_PATH = "/roms/sources.txt";
constexpr size_t      ROM_MAX_BYTES = 256 * 1024;

enum class Core { Unknown, Gameboy };

struct SupportedExt {
    const char* ext;     // lower-case, dot included
    const char* label;   // short tag shown in the picker
    Core        core;
};

// Add new cores here. The picker, dispatcher, and labels all pivot off
// this table — no other code touches the per-core mapping.
constexpr SupportedExt kSupportedExts[] = {
    { ".gb",  "GB",  Core::Gameboy },
    { ".gbc", "GBC", Core::Gameboy },
    // Future:
    // { ".nes", "NES", Core::Nes },
    // { ".sms", "SMS", Core::Sms },
};

enum class Stage { Picker, Sources, Manifest, Loading, Playing, Error };

struct PickerEntry {
    std::string filename;
    Core        core;
    const char* label;
};

struct PlayState {
    uint16_t* framebuf = nullptr;   // 160×144 RGB565 for GnuBoy
    uint8_t*  rom      = nullptr;
    size_t    romLen   = 0;
    std::string romName;
    std::string savePath;
    int       pad        = 0;
    uint32_t  lastSaveMs = 0;
};

Stage                    g_stage = Stage::Picker;
ui::list::State          g_picker;
std::vector<PickerEntry> g_entries;
ui::list::State          g_sources;   // label = display, value = manifest URL
ui::list::State          g_manifest;  // label = display, value = ROM URL
PlayState                g_play;
std::string              g_errorMsg;
bool                     g_dirty = true;

void videoCb(void*);
void audioCb(void*, size_t);

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

// "Display | URL" → split. "URL" alone → derive name from the basename.
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
    std::string lower = content.substr(0, std::min<size_t>(content.size(), 2048));
    for (auto& c : lower) c = (char)tolower(c);
    return lower.find("<!doctype") != std::string::npos ||
           lower.find("<html")     != std::string::npos ||
           lower.find("<body")     != std::string::npos ||
           lower.find("</a>")      != std::string::npos;
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

const SupportedExt* matchExt(const std::string& filename);

// Scrape an HTML index for href values pointing at any supported-extension
// ROM. Populates g_manifest in place. Returns whether anything was found.
bool extractRomLinksFromHtml(const std::string& html, const std::string& baseUrl) {
    g_manifest.items.clear();
    g_manifest.selected  = 0;
    g_manifest.scrollTop = 0;

    std::string baseDir = baseUrl;
    size_t lastSlash = baseDir.find_last_of('/');
    baseDir = (lastSlash != std::string::npos) ? baseDir.substr(0, lastSlash + 1) : baseUrl + "/";

    size_t pos = 0;
    while ((pos = html.find("<a", pos)) != std::string::npos) {
        size_t tagEnd = html.find('>', pos);
        if (tagEnd == std::string::npos) { pos++; continue; }
        std::string tag  = html.substr(pos, tagEnd - pos);
        std::string href = extractHrefValue(tag);
        if (href.empty()) { pos = tagEnd + 1; continue; }

        // Resolve relative URLs.
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

        // Strip query for extension test + display name.
        std::string filePart = fullUrl;
        auto q = filePart.find('?');
        if (q != std::string::npos) filePart = filePart.substr(0, q);
        if (!matchExt(filePart)) { pos = tagEnd + 1; continue; }

        size_t ls = filePart.find_last_of('/');
        std::string displayName = (ls != std::string::npos) ? filePart.substr(ls + 1) : filePart;

        bool dup = false;
        for (const auto& item : g_manifest.items) {
            if (item.value == fullUrl) { dup = true; break; }
        }
        if (!dup && !displayName.empty()) {
            g_manifest.items.push_back({displayName, fullUrl});
        }
        pos = tagEnd + 1;
    }
    return !g_manifest.items.empty();
}

// Plain HTTP/HTTPS GET into a string. Picks the right client based on scheme.
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

void drawDownloadProgress(const char* title, const char* sub, int pct) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(8, 8);
    d.print(title);
    d.setTextSize(1);
    d.setTextColor(0x8C71);
    d.setCursor(8, 36);
    d.print(sub);
    int barX = 8, barY = 60, barW = 224, barH = 10;
    d.drawRect(barX, barY, barW, barH, WHITE);
    if (pct >= 0) {
        int fill = (int)((barW - 2) * pct / 100);
        d.fillRect(barX + 1, barY + 1, fill, barH - 2, 0x07E0);
    }
}

const SupportedExt* matchExt(const std::string& filename) {
    if (filename.empty()) return nullptr;
    for (const auto& e : kSupportedExts) {
        size_t elen = strlen(e.ext);
        if (filename.size() < elen) continue;
        std::string tail = filename.substr(filename.size() - elen);
        for (auto& c : tail) c = (char)tolower(c);
        if (tail == e.ext) return &e;
    }
    return nullptr;
}

void scanRoms() {
    g_picker.items.clear();
    g_entries.clear();
    g_picker.selected  = 0;
    g_picker.scrollTop = 0;

    // Synthetic first row that opens the on-device downloader.
    g_picker.items.push_back({"[+ Download ROMs]", "", true});
    g_entries.push_back({"", Core::Unknown, ""});   // index-aligned placeholder

    File dir = SD.open(ROMS_DIR);
    if (dir && dir.isDirectory()) {
        File f;
        while ((f = dir.openNextFile())) {
            std::string name = f.name();
            f.close();
            auto slash = name.find_last_of('/');
            if (slash != std::string::npos) name = name.substr(slash + 1);
            const SupportedExt* ext = matchExt(name);
            if (!ext) continue;
            g_entries.push_back({name, ext->core, ext->label});
            std::string label = std::string("[") + ext->label + "] " + name;
            g_picker.items.push_back({label, "", false});
        }
        dir.close();
    }
    if (g_entries.size() <= 1) {
        g_picker.items.push_back({"(no supported ROMs in /roms)", "", false});
        g_entries.push_back({"", Core::Unknown, ""});
    }
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
    }
    if (g_sources.items.empty()) {
        g_sources.items.push_back(
            {"(no /roms/sources.txt — see README)", "", false});
    }
}

// Fetch a manifest URL. If it looks like an HTML index, scrape ROM-extension
// hrefs from it. Otherwise parse as a one-URL-per-line plain text list.
bool fetchManifestUrl(const std::string& url) {
    bool hadCanvas    = ui::canvasActive();
    bool wasBlePaused = ble::isPaused();
    if (hadCanvas)     ui::releaseCanvas();
    if (!wasBlePaused) ble::pause();
    delay(50);

    auto restore = [&]() {
        if (!wasBlePaused) ble::resume();
        if (hadCanvas)     ui::tryAcquireCanvas();
    };

    drawDownloadProgress("loading", url.c_str(), -1);

    std::string body, err;
    bool ok = httpGet(url, body, err);
    if (!ok) {
        g_errorMsg = err;
        restore();
        return false;
    }

    if (isHtmlContent(body)) {
        bool found = extractRomLinksFromHtml(body, url);
        restore();
        if (!found) { g_errorMsg = "no ROMs in HTML"; return false; }
        return true;
    }

    g_manifest.items.clear();
    g_manifest.selected  = 0;
    g_manifest.scrollTop = 0;

    size_t start = 0;
    while (start <= body.size()) {
        size_t nl = body.find('\n', start);
        std::string line = trim(body.substr(start,
            (nl == std::string::npos ? body.size() : nl) - start));
        if (!line.empty() && line[0] != '#') {
            std::string name, romUrl;
            parseManifestLine(line, name, romUrl);
            if (!romUrl.empty()) g_manifest.items.push_back({name, romUrl});
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    restore();
    if (g_manifest.items.empty()) { g_errorMsg = "manifest empty"; return false; }
    return true;
}

bool downloadRom(const std::string& name, const std::string& url) {
    if (!wifi::isConnected()) { g_errorMsg = "wifi offline"; return false; }

    bool hadCanvas    = ui::canvasActive();
    bool wasBlePaused = ble::isPaused();
    if (hadCanvas)     ui::releaseCanvas();
    if (!wasBlePaused) ble::pause();
    delay(50);

    auto restore = [&]() {
        if (!wasBlePaused) ble::resume();
        if (hadCanvas)     ui::tryAcquireCanvas();
    };

    SD.mkdir(ROMS_DIR);
    std::string fname = name;
    auto slash = fname.find_last_of('/');
    if (slash != std::string::npos) fname = fname.substr(slash + 1);
    // If the manifest entry name lost its extension, derive one from the URL.
    if (!matchExt(fname)) {
        std::string ufile = url;
        auto q = ufile.find('?');
        if (q != std::string::npos) ufile = ufile.substr(0, q);
        auto us = ufile.find_last_of('/');
        if (us != std::string::npos) {
            std::string urlName = ufile.substr(us + 1);
            if (matchExt(urlName)) fname = urlName;
        }
    }
    std::string path = std::string(ROMS_DIR) + "/" + fname;

    drawDownloadProgress("downloading", fname.c_str(), 0);

    bool secure = url.rfind("https://", 0) == 0;
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    WiFiClientSecure tls;
    WiFiClient       plain;
    bool ok;
    if (secure) { tls.setInsecure(); ok = http.begin(tls, url.c_str()); }
    else        { ok = http.begin(plain, url.c_str()); }
    if (!ok) { g_errorMsg = "http begin failed"; restore(); return false; }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[48]; snprintf(buf, sizeof(buf), "http %d", code);
        g_errorMsg = buf; http.end(); restore(); return false;
    }

    int total = http.getSize();
    if (total > 0 && (size_t)total > ROM_MAX_BYTES) {
        g_errorMsg = "ROM > 256 KB"; http.end(); restore(); return false;
    }

    File out = SD.open(path.c_str(), FILE_WRITE);
    if (!out) { g_errorMsg = "SD open failed"; http.end(); restore(); return false; }

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
            if ((size_t)(got + n) > ROM_MAX_BYTES) {
                g_errorMsg = "ROM > 256 KB";
                out.close(); SD.remove(path.c_str()); http.end(); restore();
                return false;
            }
            out.write(buf, n);
            got += n;
            if (total > 0) {
                int pct = (int)((int64_t)got * 100 / total);
                if (pct != lastPct) { drawDownloadProgress("downloading", fname.c_str(), pct); lastPct = pct; }
            }
            deadline = millis() + 60000;
        } else if (millis() > deadline) {
            g_errorMsg = "download timeout";
            out.close(); SD.remove(path.c_str()); http.end(); restore();
            return false;
        } else {
            delay(1);
        }
    }
    out.close();
    http.end();

    if (got == 0) {
        g_errorMsg = "empty download";
        SD.remove(path.c_str()); restore(); return false;
    }
    restore();
    return true;
}

bool loadRom(const std::string& filename) {
    std::string path = std::string(ROMS_DIR) + "/" + filename;
    File f = SD.open(path.c_str());
    if (!f) { g_errorMsg = "open failed: " + filename; return false; }
    size_t sz = f.size();
    if (sz == 0 || sz > ROM_MAX_BYTES) {
        char buf[64];
        snprintf(buf, sizeof(buf), "rom %u bytes > %u cap",
                 (unsigned)sz, (unsigned)ROM_MAX_BYTES);
        g_errorMsg = buf;
        f.close();
        return false;
    }
    g_play.rom = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
    if (!g_play.rom) {
        char buf[64];
        snprintf(buf, sizeof(buf), "alloc %u failed (largest=%u)",
                 (unsigned)sz, (unsigned)ESP.getMaxAllocHeap());
        g_errorMsg = buf;
        f.close();
        return false;
    }
    size_t read = f.read(g_play.rom, sz);
    f.close();
    if (read != sz) {
        g_errorMsg = "short read";
        heap_caps_free(g_play.rom);
        g_play.rom = nullptr;
        return false;
    }
    g_play.romLen  = sz;
    g_play.romName = filename;
    return true;
}

bool allocGbFramebuf() {
    size_t fbBytes = GB_WIDTH * GB_HEIGHT * sizeof(uint16_t);
    g_play.framebuf = (uint16_t*)heap_caps_malloc(
        fbBytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!g_play.framebuf) { g_errorMsg = "framebuf alloc failed"; return false; }
    memset(g_play.framebuf, 0, fbBytes);
    return true;
}

void freePlayState() {
    if (g_play.rom)      { heap_caps_free(g_play.rom);      g_play.rom      = nullptr; }
    if (g_play.framebuf) { heap_caps_free(g_play.framebuf); g_play.framebuf = nullptr; }
    g_play.romLen = 0;
    g_play.romName.clear();
    g_play.savePath.clear();
    g_play.pad = 0;
}

void videoCb(void* /*buffer*/) {
    auto& d = M5Cardputer.Display;
    const int srcSkipTop  = 5;
    const int displayRows = 135;
    d.pushImage(40, 0, GB_WIDTH, displayRows,
                g_play.framebuf + srcSkipTop * GB_WIDTH);
}

void audioCb(void* /*buffer*/, size_t /*length*/) {
    // No sound in v1; GnuBoy still calls this but we discard the samples.
}

void translateKey(char ch, bool down) {
    int bit = 0;
    switch (ch) {
        case 'e': case 'E': bit = GB_PAD_UP;     break;
        case 's': case 'S': bit = GB_PAD_DOWN;   break;
        case 'a': case 'A': bit = GB_PAD_LEFT;   break;
        case 'd': case 'D': bit = GB_PAD_RIGHT;  break;
        case 'k': case 'K': bit = GB_PAD_B;      break;
        case 'l': case 'L': bit = GB_PAD_A;      break;
        case '1':           bit = GB_PAD_START;  break;
        case '2':           bit = GB_PAD_SELECT; break;
        case key::Up:    bit = GB_PAD_UP;    break;
        case key::Down:  bit = GB_PAD_DOWN;  break;
        case key::Left:  bit = GB_PAD_LEFT;  break;
        case key::Right: bit = GB_PAD_RIGHT; break;
        default: return;
    }
    if (down) g_play.pad |= bit;
    else      g_play.pad &= ~bit;
    gnuboy_set_pad(g_play.pad);
}

void runGnuboyLoop() {
    constexpr int targetFps = 60;
    constexpr uint32_t frameUs = 1000000u / targetFps;
    uint64_t nextFrame = esp_timer_get_time();

    uint32_t frameCount = 0;
    uint32_t lastLogMs  = millis();

    while (g_stage == Stage::Playing) {
        gnuboy_run(true);

        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange()) {
            auto kstate = M5Cardputer.Keyboard.keysState();
            g_play.pad = 0;
            for (char ch : kstate.word) translateKey(ch, true);
            for (char ch : kstate.word) if (ch == '\t') g_stage = Stage::Picker;
        }

        uint32_t nowMs = millis();
        if (nowMs - g_play.lastSaveMs > 5000 && gnuboy_sram_dirty()) {
            gnuboy_save_sram(g_play.savePath.c_str(), true);
            g_play.lastSaveMs = nowMs;
        }

        frameCount++;
        if (nowMs - lastLogMs >= 1000) {
            Serial.printf("[retro/gb] fps=%u heap_free=%u\n",
                          (unsigned)frameCount, (unsigned)ESP.getFreeHeap());
            frameCount = 0;
            lastLogMs  = nowMs;
        }

        nextFrame += frameUs;
        int64_t now = (int64_t)esp_timer_get_time();
        int64_t late = now - (int64_t)nextFrame;
        if (late > (int64_t)frameUs) {
            nextFrame = (uint64_t)now;
        } else if (late < 0) {
            uint32_t sleepUs = (uint32_t)(-late);
            if (sleepUs > 1500) delay(sleepUs / 1000);
        }
    }
}

void runGameboyCore(const std::string& filename) {
    if (!allocGbFramebuf()) {
        g_stage = Stage::Error; g_dirty = true; freePlayState();
        return;
    }

    if (ui::canvasActive()) ui::releaseCanvas();
    if (!ble::isPaused())   ble::pause();

    int rc = gnuboy_init(32000, GB_AUDIO_MONO_S16, GB_PIXEL_565_LE,
                         &videoCb, &audioCb);
    if (rc < 0) { g_errorMsg = "gnuboy_init failed"; goto fail; }

    gnuboy_set_framebuffer(g_play.framebuf);
    rc = gnuboy_load_rom(g_play.rom, g_play.romLen);
    if (rc < 0) { g_errorMsg = "rom load failed"; goto fail; }

    SD.mkdir(SAVES_DIR);
    g_play.savePath = std::string(SAVES_DIR) + "/" + filename + ".sav";
    gnuboy_load_sram(g_play.savePath.c_str());

    gnuboy_reset(true);
    gnuboy_set_pad(0);
    M5Cardputer.Display.fillScreen(BLACK);

    g_stage = Stage::Playing;
    runGnuboyLoop();

    if (gnuboy_sram_dirty()) gnuboy_save_sram(g_play.savePath.c_str(), false);
    gnuboy_free_rom();
    freePlayState();
    ble::resume();
    ui::tryAcquireCanvas();
    g_dirty = true;
    return;

fail:
    g_stage = Stage::Error;
    g_dirty = true;
    freePlayState();
    if (ble::isPaused()) ble::resume();
    ui::tryAcquireCanvas();
}

void dispatch(Core core, const std::string& filename) {
    switch (core) {
        case Core::Gameboy: runGameboyCore(filename); break;
        case Core::Unknown: g_errorMsg = "no core for this rom"; g_stage = Stage::Error; g_dirty = true; break;
    }
}

void startSelected() {
    if (g_entries.empty() || g_picker.selected >= (int)g_entries.size()) return;

    // Picker row 0 is the synthetic "[+ Download ROMs]" sentinel.
    if (g_picker.selected == 0) {
        buildSourcesList();
        g_stage = Stage::Sources;
        g_dirty = true;
        return;
    }

    const auto& entry = g_entries[g_picker.selected];
    if (entry.filename.empty()) return;   // info row

    g_stage = Stage::Loading;
    g_dirty = true;
    g_errorMsg.clear();
    freePlayState();

    if (!loadRom(entry.filename)) {
        g_stage = Stage::Error;
        g_dirty = true;
        return;
    }
    dispatch(entry.core, entry.filename);
}

void enterManifest() {
    if (g_sources.items.empty()) return;
    const auto& item = g_sources.items[g_sources.selected];
    if (item.value.empty()) return;   // info row
    g_errorMsg.clear();
    if (fetchManifestUrl(item.value)) {
        g_stage = Stage::Manifest;
    } else {
        g_stage = Stage::Error;
    }
    g_dirty = true;
}

void downloadSelected() {
    if (g_manifest.items.empty()) return;
    const auto& item = g_manifest.items[g_manifest.selected];
    if (item.value.empty()) return;
    g_errorMsg.clear();
    if (downloadRom(item.label, item.value)) {
        // Back to picker so the new ROM is visible.
        scanRoms();
        g_stage = Stage::Picker;
    } else {
        g_stage = Stage::Error;
    }
    g_dirty = true;
}

void renderPicker() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setTextSize(1);
    d.setTextColor(0xFFE0);
    d.setCursor(6, 18);
    d.print("Retro — pick a game");
    ui::list::draw(g_picker, 6, 32, 228, 90);
    d.setCursor(6, 124);
    d.setTextColor(0x8C71);
    d.print("enter play   tab home");
    ui::flush();
}

void renderSources() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setTextSize(1);
    d.setTextColor(0xFFE0);
    d.setCursor(6, 18);
    d.print("ROM sources");
    ui::list::draw(g_sources, 6, 32, 228, 90);
    d.setCursor(6, 124);
    d.setTextColor(0x8C71);
    d.print("enter fetch   bksp back");
    ui::flush();
}

void renderManifest() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setTextSize(1);
    d.setTextColor(0xFFE0);
    d.setCursor(6, 18);
    d.print("pick ROM to download");
    ui::list::draw(g_manifest, 6, 32, 228, 90);
    d.setCursor(6, 124);
    d.setTextColor(0x8C71);
    d.print("enter download   bksp back");
    ui::flush();
}

void renderLoading() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setCursor(6, 60);
    d.setTextColor(0xFFE0);
    d.print("loading ROM…");
    ui::flush();
}

void renderError() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setCursor(6, 50);
    d.setTextColor(0xF800);
    d.print("error:");
    d.setCursor(6, 64);
    d.setTextColor(0xFFE0);
    d.print(g_errorMsg.c_str());
    d.setCursor(6, 90);
    d.setTextColor(0x8C71);
    d.print("any key: back");
    ui::flush();
}

void onEnter() {
    g_stage = Stage::Picker;
    scanRoms();
    g_dirty = true;
}

void onExit() {
    if (g_stage == Stage::Playing) {
        freePlayState();
        if (ble::isPaused()) ble::resume();
        ui::tryAcquireCanvas();
    }
}

void onTick() {}

void onKey(char ch) {
    switch (g_stage) {
        case Stage::Picker:
            if (ch == '\n') startSelected();
            else if (ui::list::onKey(g_picker, ch)) g_dirty = true;
            break;
        case Stage::Sources:
            if (ch == '\n')      enterManifest();
            else if (ch == '\b') { g_stage = Stage::Picker;  g_dirty = true; }
            else if (ui::list::onKey(g_sources, ch)) g_dirty = true;
            break;
        case Stage::Manifest:
            if (ch == '\n')      downloadSelected();
            else if (ch == '\b') { g_stage = Stage::Sources; g_dirty = true; }
            else if (ui::list::onKey(g_manifest, ch)) g_dirty = true;
            break;
        case Stage::Playing:
            break;
        case Stage::Loading:
            break;
        case Stage::Error:
            g_stage = Stage::Picker;
            g_dirty = true;
            break;
    }
}

void onDraw() {
    if (!g_dirty) return;
    switch (g_stage) {
        case Stage::Picker:   renderPicker();   break;
        case Stage::Sources:  renderSources();  break;
        case Stage::Manifest: renderManifest(); break;
        case Stage::Loading:  renderLoading();  break;
        case Stage::Playing:  break;
        case Stage::Error:    renderError();    break;
    }
    g_dirty = false;
}

App retro_app = {
    .id           = "retro",
    .name         = "Retro",
    .description  = "Game console emulators",
    .services     = SVC_WIFI | SVC_SD | SVC_CANVAS,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .onDraw       = onDraw,
    .onEvent      = nullptr,
    .keysAsArrows = true,
    .hidden       = false,
};

}  // namespace

REGISTER_APP(retro_app);
