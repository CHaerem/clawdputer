// Game Boy emulator app — loads .gb ROMs from microSD and runs them via Peanut-GB.
//
// Stage::Picker  — scrollable file browser of .gb files on SD root and /roms/
// Stage::Playing — emulation loop; Tab exits to home (caught by main.cpp)
//
// Controls during play:
//   E/S/A/D = Up/Down/Left/Right   K = B   L = A   1/Enter = Start   2 = Select

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SD.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/app.h"
#include "services/sd.h"
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

enum class Stage { Picker, Playing };

Stage           g_stage = Stage::Picker;
ui::list::State g_picker;

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

void buildFileList() {
    g_picker.items.clear();
    g_picker.selected  = 0;
    g_picker.scrollTop = 0;

    auto addDir = [](const char* path) {
        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) return;
        File f;
        while ((f = dir.openNextFile())) {
            std::string name = f.name();
            if (!f.isDirectory() && name.size() > 3 &&
                name.substr(name.size() - 3) == ".gb") {
                std::string full = std::string(path) + "/" + name;
                // Normalise double slashes from root "/"
                if (full.size() > 1 && full[0] == '/' && full[1] == '/')
                    full = full.substr(1);
                g_picker.items.push_back({name, full});
            }
            f.close();
        }
    };

    addDir("/");
    addDir("/roms");
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
}

void drawPicker() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    if (g_picker.items.empty()) {
        d.setTextSize(1);
        d.setTextColor(0x7BEF);
        d.setCursor(8, SCREEN_H / 2 - 4);
        d.print("No .gb files on SD card");
    } else {
        d.setTextSize(1);
        d.setTextColor(0xFFFF);
        d.setCursor(4, ui::statusbar::HEIGHT + 2);
        d.print("Select ROM");
        ui::list::draw(g_picker, 0, ui::statusbar::HEIGHT + 12,
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
    if (g_stage == Stage::Picker) {
        drawPicker();
        return;
    }

    // Playing
    uint32_t now = millis();
    if (now - g_lastFrameMs < 16) return;
    g_lastFrameMs = now;

    // Read full keyboard state each frame for held-key support
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
    if (g_stage != Stage::Picker) return;

    if (ch == '\n' && !g_picker.items.empty()) {
        const std::string& path = g_picker.items[g_picker.selected].value;
        if (launchRom(path)) g_stage = Stage::Playing;
        return;
    }

    ui::list::onKey(g_picker, ch);
}

static App gameboy_app = {
    .id           = "gameboy",
    .name         = "Game Boy",
    .description  = "GB emulator — ROMs on SD card",
    .services     = SVC_SD,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .keysAsArrows = false,
};
REGISTER_APP(gameboy_app);

}  // namespace
