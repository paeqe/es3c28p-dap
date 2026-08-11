#pragma once
#include <Arduino.h>
#include <Wire.h>

// Minimal ES8311 driver for playback only (DAC path).
// Assumes the ESP32 is I2S master and supplies MCLK at 256 x sample rate,
// which is what the audio library requests by default.
class ES8311 {
public:
    bool begin(TwoWire &wire, uint8_t addr, uint32_t sampleRate = 44100);

    // 0 = mute, 100 = full scale. Maps onto the codec's DAC volume register.
    void setVolume(uint8_t percent);
    void mute(bool on);

    // Call after changing sample rate mid-stream.
    bool setSampleRate(uint32_t sampleRate);

    bool present();

private:
    TwoWire *_wire = nullptr;
    uint8_t  _addr = 0x18;

    bool    writeReg(uint8_t reg, uint8_t val);
    uint8_t readReg(uint8_t reg);
};
