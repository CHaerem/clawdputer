#include <M5Cardputer.h>

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(WHITE);
    M5Cardputer.Display.setCursor(8, 8);
    M5Cardputer.Display.println("clawdputer");
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(8, 40);
    M5Cardputer.Display.println("boot ok");
}

void loop() {
    M5Cardputer.update();
}
