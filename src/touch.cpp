#include "touch.h"
#include "board_config.h"
#include <Wire.h>

namespace {

constexpr uint8_t REG_NUM_TOUCHES = 0x02;
constexpr uint8_t REG_P1_XH       = 0x03;

uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((int)TP_I2C_ADDR, 1) != 1) return 0;
    return Wire.read();
}

} // namespace

namespace touch {

bool begin() {
    pinMode(PIN_TP_RST, OUTPUT);
    digitalWrite(PIN_TP_RST, LOW);
    delay(10);
    digitalWrite(PIN_TP_RST, HIGH);
    delay(300);                     // FT6336 needs time before it answers

    pinMode(PIN_TP_INT, INPUT_PULLUP);

    Wire.beginTransmission(TP_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("[touch] FT6336 not responding");
        return false;
    }

    // 0xA4: interrupt mode. 0x00 = polling, which is what read() expects.
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(0xA4);
    Wire.write(0x00);
    Wire.endTransmission();

    // 0x80: touch threshold. Lower is more sensitive; 22 is the usual default.
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(0x80);
    Wire.write(22);
    Wire.endTransmission();

    // 0xA3 is the chip ID, 0xA8 the vendor ID.
    Serial.printf("[touch] ok (chip 0x%02X, vendor 0x%02X)\n",
                  readReg(0xA3), readReg(0xA8));
    return true;
}

bool readRaw(int32_t &x, int32_t &y, uint8_t &fingers) {
    fingers = readReg(REG_NUM_TOUCHES) & 0x0F;
    if (fingers == 0 || fingers > 2) return false;

    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(REG_P1_XH);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)TP_I2C_ADDR, 4) != 4) return false;

    uint8_t xh = Wire.read();
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read();
    uint8_t yl = Wire.read();

    // Top two bits of xh are the event flag, not coordinate data.
    x = ((xh & 0x0F) << 8) | xl;
    y = ((yh & 0x0F) << 8) | yl;
    return true;
}

// Adjust these three if the pointer does not follow your finger.
// See the debug screen for what each one does.
bool gSwapXY  = false;
bool gInvertX = false;
bool gInvertY = false;

void setTransform(bool swapXY, bool invertX, bool invertY) {
    gSwapXY  = swapXY;
    gInvertX = invertX;
    gInvertY = invertY;
}

bool read(int32_t &x, int32_t &y) {
    uint8_t fingers;
    if (!readRaw(x, y, fingers)) return false;

    if (gSwapXY) { int32_t t = x; x = y; y = t; }
    if (gInvertX) x = (LCD_WIDTH  - 1) - x;
    if (gInvertY) y = (LCD_HEIGHT - 1) - y;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= LCD_WIDTH)  x = LCD_WIDTH  - 1;
    if (y >= LCD_HEIGHT) y = LCD_HEIGHT - 1;
    return true;
}

void scanBus() {
    Serial.println("[i2c] scanning...");
    uint8_t found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[i2c]   device at 0x%02X\n", addr);
            found++;
        }
    }
    if (!found) Serial.println("[i2c]   nothing found");
}

} // namespace touch
