#include "tonetest.h"
#include "board_config.h"
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <math.h>

namespace {

// I2S_NUM_0 belongs to the audio library: its Audio object is a global in
// player.cpp, so its constructor installs a driver on port 0 before setup()
// ever runs and i2s_driver_install() here would fail with INVALID_STATE.
// Port 1 is free, and keeping the test on its own driver is the point of it --
// a config we control end to end, so silence really does mean codec or amp.
constexpr i2s_port_t I2S_PORT    = I2S_NUM_1;
constexpr uint32_t   SAMPLE_RATE = 44100;

void writeTone(float freqHz, uint32_t ms, float amplitude) {
    constexpr size_t CHUNK = 256;              // frames per write
    int16_t buf[CHUNK * 2];                    // stereo interleaved

    const uint32_t totalFrames = (SAMPLE_RATE * ms) / 1000;
    const float    step        = 2.0f * PI * freqHz / SAMPLE_RATE;

    static float phase = 0.0f;
    uint32_t written = 0;

    while (written < totalFrames) {
        size_t frames = min((size_t)(totalFrames - written), CHUNK);

        for (size_t i = 0; i < frames; i++) {
            int16_t s = (int16_t)(sinf(phase) * 32767.0f * amplitude);
            buf[i * 2]     = s;                // left
            buf[i * 2 + 1] = s;                // right
            phase += step;
            if (phase > 2.0f * PI) phase -= 2.0f * PI;
        }

        size_t bytesWritten = 0;
        i2s_write(I2S_PORT, buf, frames * 4, &bytesWritten, portMAX_DELAY);
        written += frames;
    }
}

// Puts a pin back to plain GPIO, driven low. i2s_driver_uninstall() does not
// undo the GPIO matrix routing, so without this the pins would keep carrying
// I2S1's (now stopped) signals afterwards.
void detach(int pin) {
    if (pin < 0) return;
    gpio_reset_pin((gpio_num_t)pin);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

} // namespace

namespace tonetest {

void run() {
    Serial.println("[tone] starting test tone");

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    // The ES8311 needs MCLK, and the codec driver assumes 256 x fs.
    cfg.mclk_multiple        = I2S_MCLK_MULTIPLE_256;

    esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[tone] i2s_driver_install failed: %s\n", esp_err_to_name(err));
        return;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = PIN_I2S_MCLK;
    pins.bck_io_num   = PIN_I2S_BCLK;
    pins.ws_io_num    = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
        Serial.println("[tone] i2s_set_pin failed");
        i2s_driver_uninstall(I2S_PORT);
        return;
    }

    i2s_zero_dma_buffer(I2S_PORT);

    // A short rising scale is easier to recognise through a small speaker
    // than a single sustained tone.
    const float notes[] = {440.0f, 554.37f, 659.25f, 880.0f};
    for (float f : notes) {
        Serial.printf("[tone]   %.0f Hz\n", f);
        writeTone(f, 400, 0.35f);
    }

    // Then a slow sweep, which makes any distortion or dropout obvious.
    Serial.println("[tone]   sweep 200-2000 Hz");
    for (int i = 0; i < 60; i++) {
        float f = 200.0f + (i * 30.0f);
        writeTone(f, 50, 0.35f);
    }

    i2s_zero_dma_buffer(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);
    Serial.println("[tone] done");
}

void runDac() {
    Serial.println("[dactone] test tone straight at the PCM510x pins");
    Serial.printf("[dactone] BCK=IO%d LCK=IO%d DIN=IO%d, no MCLK (module's SCK must be grounded)\n",
                  PIN_DAC_BCLK, PIN_DAC_LRCK, PIN_DAC_DOUT);

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    // No mclk_multiple here: the PCM510x runs its own PLL off BCK and is given
    // no master clock at all, which is the whole point of grounding SCK.

    esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[dactone] i2s_driver_install failed: %s\n", esp_err_to_name(err));
        return;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_DAC_BCLK;
    pins.ws_io_num    = PIN_DAC_LRCK;
    pins.data_out_num = PIN_DAC_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[dactone] i2s_set_pin failed: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(I2S_PORT);
        return;
    }

    i2s_zero_dma_buffer(I2S_PORT);

    const float notes[] = {440.0f, 554.37f, 659.25f, 880.0f};
    for (float f : notes) {
        Serial.printf("[dactone]   %.0f Hz\n", f);
        writeTone(f, 400, 0.35f);
    }

    i2s_zero_dma_buffer(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);
    detach(PIN_DAC_BCLK);
    detach(PIN_DAC_LRCK);
    detach(PIN_DAC_DOUT);
    Serial.println("[dactone] done -- heard it? then the module and wiring are fine");
}

} // namespace tonetest
