// Retro game console emulator app — single home for all vendored cores.
// Currently ships only GnuBoy (GB / GBC). Future cores (NES via Nofrendo,
// SMS/GG via SMSPlus, NGP via Race, …) plug in here as siblings: add an
// entry to kSupportedExts, a Core enum value, and a case in dispatch().
//
// ROMs live under /roms/ on the SD card. Two ways to get them there:
//   - On-device: [+ Download games] in the picker pulls the project's
//     public manifest from GitHub Pages and lets you pick a ROM to fetch.
//     Only public-domain titles (Blargg test ROMs by default — edit
//     web/roms-manifest.txt in the repo to extend the list).
//   - From the host: tools/sync-roms.sh mirrors the same manifest plus
//     an optional gitignored private manifest (commercial ROMs you own,
//     hosted on your own private storage) to a target directory.
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
constexpr const char* MANIFEST_URL =
    "https://chaerem.github.io/clawdputer/roms-manifest.txt";
constexpr size_t      ROM_MAX_BYTES = 256 * 1024;

enum class Core { Unknown, Gameboy };

struct SupportedExt {
    const char* ext;     // lower-case, dot included
    const char* label;   // short tag shown in the picker
    Core        core;
};

constexpr SupportedExt kSupportedExts[] = {
    { ".gb",  "GB",  Core::Gameboy },
    { ".gbc", "GBC", Core::Gameboy },
    // Future:
    // { ".nes", "NES", Core::Nes },
    // { ".sms", "SMS", Core::Sms },
};

enum class Stage { Picker, Manifest, Loading, Playing, Error };

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
ui::list::State          g_manifest;   // value = ROM URL
PlayState                g_play;
std::string              g_errorMsg;
bool                     g_dirty = true;

void videoCb(void*);
void audioCb(void*, size_t);

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

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

// "filename.gb | URL" → split. Bare URL → derive name from URL basename.
void parseManifestLine(const std::string& raw,
                       std::string& nameOut, std::string& urlOut) {
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

void scanRoms() {
    g_picker.items.clear();
    g_entries.clear();
    g_picker.selected  = 0;
    g_picker.scrollTop = 0;

    // Synthetic first row opens the on-device downloader.
    g_picker.items.push_back({"[+ Download games]", "", true});
    g_entries.push_back({"", Core::Unknown, ""});

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
        g_picker.items.push_back({"(no ROMs yet — use download above)", "", false});
        g_entries.push_back({"", Core::Unknown, ""});
    }
}

void drawProgress(const char* title, const char* sub, int pct) {
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

// Pause BLE + release canvas during HTTPS — mbedTLS handshake needs a
// chunk of contiguous heap that our normal-mode allocations don't leave
// free. Caller is responsible for invoking the returned restore function.
struct UiPause {
    bool hadCanvas;
    bool wasBlePaused;
};
UiPause pauseUi() {
    UiPause s{ ui::canvasActive(), ble::isPaused() };
    if (s.hadCanvas)      ui::releaseCanvas();
    if (!s.wasBlePaused)  ble::pause();
    delay(50);
    return s;
}
void restoreUi(const UiPause& s) {
    if (!s.wasBlePaused) ble::resume();
    if (s.hadCanvas)     ui::tryAcquireCanvas();
}

bool httpGetString(const std::string& url,
                   std::string& bodyOut, std::string& errOut) {
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

bool fetchManifest() {
    UiPause guard = pauseUi();
    drawProgress("loading manifest", MANIFEST_URL, -1);

    std::string body, err;
    bool ok = httpGetString(MANIFEST_URL, body, err);
    restoreUi(guard);
    if (!ok) { g_errorMsg = err; return false; }

    g_manifest.items.clear();
    g_manifest.selected  = 0;
    g_manifest.scrollTop = 0;

    size_t start = 0;
    while (start <= body.size()) {
        size_t nl = body.find('\n', start);
        std::string line = trim(body.substr(start,
            (nl == std::string::npos ? body.size() : nl) - start));
        if (!line.empty() && line[0] != '#') {
            std::string name, url;
            parseManifestLine(line, name, url);
            if (!url.empty() && matchExt(name))
                g_manifest.items.push_back({name, url});
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (g_manifest.items.empty()) {
        g_errorMsg = "manifest has no supported ROMs";
        return false;
    }
    return true;
}

bool downloadRom(const std::string& filename, const std::string& url) {
    if (!wifi::isConnected()) { g_errorMsg = "wifi offline"; return false; }

    UiPause guard = pauseUi();
    SD.mkdir(ROMS_DIR);
    std::string path = std::string(ROMS_DIR) + "/" + filename;

    drawProgress("downloading", filename.c_str(), 0);

    bool secure = url.rfind("https://", 0) == 0;
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    WiFiClientSecure tls;
    WiFiClient       plain;
    bool ok;
    if (secure) { tls.setInsecure(); ok = http.begin(tls, url.c_str()); }
    else        { ok = http.begin(plain, url.c_str()); }
    if (!ok) { g_errorMsg = "http begin failed"; restoreUi(guard); return false; }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[48]; snprintf(buf, sizeof(buf), "http %d", code);
        g_errorMsg = buf; http.end(); restoreUi(guard); return false;
    }

    int total = http.getSize();
    if (total > 0 && (size_t)total > ROM_MAX_BYTES) {
        g_errorMsg = "ROM > 256 KB"; http.end(); restoreUi(guard); return false;
    }

    File out = SD.open(path.c_str(), FILE_WRITE);
    if (!out) {
        g_errorMsg = "SD open failed";
        http.end(); restoreUi(guard); return false;
    }

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
                out.close(); SD.remove(path.c_str()); http.end();
                restoreUi(guard);
                return false;
            }
            out.write(buf, n);
            got += n;
            if (total > 0) {
                int pct = (int)((int64_t)got * 100 / total);
                if (pct != lastPct) {
                    drawProgress("downloading", filename.c_str(), pct);
                    lastPct = pct;
                }
            }
            deadline = millis() + 60000;
        } else if (millis() > deadline) {
            g_errorMsg = "download timeout";
            out.close(); SD.remove(path.c_str()); http.end();
            restoreUi(guard);
            return false;
        } else {
            delay(1);
        }
    }
    out.close();
    http.end();

    if (got == 0) {
        g_errorMsg = "empty download";
        SD.remove(path.c_str());
        restoreUi(guard);
        return false;
    }
    restoreUi(guard);
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
        case Core::Unknown:
            g_errorMsg = "no core for this rom";
            g_stage    = Stage::Error;
            g_dirty    = true;
            break;
    }
}

void startSelected() {
    if (g_entries.empty() || g_picker.selected >= (int)g_entries.size()) return;

    // Picker row 0 is the synthetic "[+ Download games]" sentinel.
    if (g_picker.selected == 0) {
        g_errorMsg.clear();
        if (fetchManifest()) {
            g_stage = Stage::Manifest;
        } else {
            g_stage = Stage::Error;
        }
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

void downloadFromManifest() {
    if (g_manifest.items.empty()) return;
    const auto& item = g_manifest.items[g_manifest.selected];
    if (item.value.empty()) return;

    g_errorMsg.clear();
    if (downloadRom(item.label, item.value)) {
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

void renderManifest() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setTextSize(1);
    d.setTextColor(0xFFE0);
    d.setCursor(6, 18);
    d.print("Pick a ROM to download");
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
        case Stage::Manifest:
            if (ch == '\n')      downloadFromManifest();
            else if (ch == '\b') { g_stage = Stage::Picker; g_dirty = true; }
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
