#include "es8311.h"

// Register names follow the ES8311 datasheet.
#define REG_RESET       0x00
#define REG_CLK_MGR_01  0x01
#define REG_CLK_MGR_02  0x02
#define REG_CLK_MGR_03  0x03
#define REG_CLK_MGR_04  0x04
#define REG_CLK_MGR_05  0x05
#define REG_CLK_MGR_06  0x06
#define REG_CLK_MGR_07  0x07
#define REG_CLK_MGR_08  0x08
#define REG_SDP_IN      0x09
#define REG_SDP_OUT     0x0A
#define REG_SYSTEM_0B   0x0B
#define REG_SYSTEM_0C   0x0C
#define REG_SYSTEM_0D   0x0D
#define REG_SYSTEM_0E   0x0E
#define REG_SYSTEM_0F   0x0F
#define REG_SYSTEM_10   0x10
#define REG_SYSTEM_11   0x11
#define REG_SYSTEM_12   0x12
#define REG_SYSTEM_13   0x13
#define REG_SYSTEM_14   0x14
#define REG_DAC_31      0x31
#define REG_DAC_VOLUME  0x32
#define REG_CHIP_ID1    0xFD
#define REG_CHIP_ID2    0xFE

bool ES8311::writeReg(uint8_t reg, uint8_t val) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(val);
    return _wire->endTransmission() == 0;
}

uint8_t ES8311::readReg(uint8_t reg) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) return 0;
    if (_wire->requestFrom((int)_addr, 1) != 1) return 0;
    return _wire->read();
}

bool ES8311::present() {
    _wire->beginTransmission(_addr);
    return _wire->endTransmission() == 0;
}

bool ES8311::begin(TwoWire &wire, uint8_t addr, uint32_t sampleRate) {
    _wire = &wire;
    _addr = addr;

    if (!present()) {
        Serial.println("[es8311] not responding on I2C");
        return false;
    }

    // --- reset ---
    writeReg(REG_RESET, 0x1F);
    delay(20);
    writeReg(REG_RESET, 0x00);
    writeReg(REG_RESET, 0x80);      // power up, slave mode
    delay(10);

    // --- clock manager: MCLK from pin, enable DAC clocks ---
    writeReg(REG_CLK_MGR_01, 0x30); // MCLK source = MCLK pin, DAC clock on
    writeReg(REG_CLK_MGR_02, 0x00); // pre-divider 1, pre-multiplier 1
    writeReg(REG_CLK_MGR_03, 0x10); // ADC oversample (unused but keep sane)
    writeReg(REG_CLK_MGR_04, 0x10); // DAC oversample
    writeReg(REG_CLK_MGR_05, 0x00); // ADC/DAC dividers = 1

    // Slave mode: clear the master-mode bit.
    writeReg(REG_RESET, readReg(REG_RESET) & 0xBF);

    writeReg(REG_CLK_MGR_01, 0x3F); // enable all clock branches

    // --- serial audio port: I2S, 16-bit ---
    writeReg(REG_SDP_IN,  0x0C);    // I2S format, 16 bit, DAC input
    writeReg(REG_SDP_OUT, 0x0C);

    // --- analogue power up ---
    writeReg(REG_SYSTEM_0D, 0x01);
    writeReg(REG_SYSTEM_0E, 0x02);
    writeReg(REG_SYSTEM_12, 0x00);  // enable DAC output
    writeReg(REG_SYSTEM_13, 0x10);  // headphone/line driver on
    writeReg(REG_SYSTEM_0B, 0x00);
    writeReg(REG_SYSTEM_0C, 0x00);
    writeReg(REG_SYSTEM_10, 0x1F);
    writeReg(REG_SYSTEM_11, 0x7F);

    writeReg(REG_DAC_31, 0x00);     // un-mute DAC

    setSampleRate(sampleRate);
    setVolume(60);

    Serial.printf("[es8311] init ok (chip id %02X%02X)\n",
                  readReg(REG_CHIP_ID1), readReg(REG_CHIP_ID2));
    return true;
}

bool ES8311::setSampleRate(uint32_t sampleRate) {
    // With MCLK locked to 256 x fs the divider chain is identical for every
    // standard rate, so only the LRCK/BCLK dividers below need writing.
    // Values below match Espressif's own ES8311 driver coefficient table
    // for the 256x row (mclk=11289600/rate=44100 and mclk=12288000/rate=48000
    // both use lrck_h=0x00, lrck_l=0xFF, bclk_div=0x04) -- both fields are
    // written as-is, not N-1 encoded.
    writeReg(REG_CLK_MGR_07, 0x00); // LRCK divider high byte
    writeReg(REG_CLK_MGR_08, 0xFF); // LRCK divider low byte
    writeReg(REG_CLK_MGR_06, 0x04); // BCLK divider
    return true;
}

void ES8311::setVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    // Register 0x32: 0x00 = mute, 0xFF = +32 dB, 0xBF (191) = 0 dB, 0.5dB/step.
    // Old range (0x40-0xE0) didn't reach 0 dB until ~80% up the slider, so
    // the old 60% default -- and most of the slider's usable travel -- sat
    // well into attenuation, which is what made playback sound quiet.
    // 0x7F-0xFF is symmetric around 0 dB: 50% is unity, below that attenuates
    // down to -32dB, above it boosts up to +32dB. Amp headroom is unmeasured,
    // so this leans conservative rather than parking the default near max gain.
    uint8_t reg = (percent == 0) ? 0x00 : map(percent, 1, 100, 0x7F, 0xFF);
    writeReg(REG_DAC_VOLUME, reg);
}

void ES8311::mute(bool on) {
    writeReg(REG_DAC_31, on ? 0x60 : 0x00);
}
