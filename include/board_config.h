#pragma once

// ---------------------------------------------------------------------------
// ES3C28P  --  2.8" ESP32-S3 display board (ILI9341V + FT6336G + ES8311)
// Pin map transcribed from the lcdwiki IO resource table.
// ---------------------------------------------------------------------------

// LCD (4-wire SPI, ILI9341V)
#define PIN_LCD_CS      10
#define PIN_LCD_DC      46
#define PIN_LCD_SCK     12
#define PIN_LCD_MOSI    11
#define PIN_LCD_MISO    13
#define PIN_LCD_BL      45      // high = backlight on
// LCD reset shares the ESP32-S3 EN line, so there is no separate GPIO.
#define PIN_LCD_RST     -1

#define LCD_WIDTH       240
#define LCD_HEIGHT      320

// Capacitive touch (FT6336G, I2C)
#define PIN_TP_SDA      16
#define PIN_TP_SCL      15
#define PIN_TP_RST      18
#define PIN_TP_INT      17
#define TP_I2C_ADDR     0x38

// The ES8311 codec sits on the same I2C bus as the touch controller.
#define ES8311_I2C_ADDR 0x18

// Audio (I2S to ES8311 -> FM8002E amplifier -> speaker header)
// Pin assignment verified against Freenove's Sketch_07.1_Music reference.
// DOUT and DIN are easy to get backwards: DOUT (8) carries playback data from
// the ESP32 to the codec, DIN (6) carries microphone data the other way.
#define PIN_PA_EN       1       // LOW enables the amplifier
#define PIN_I2S_MCLK    4
#define PIN_I2S_BCLK    5
#define PIN_I2S_LRCK    7
#define PIN_I2S_DOUT    8       // ESP32 -> codec (playback)
#define PIN_I2S_DIN     6       // codec -> ESP32 (microphone)

// External I2S DAC on a GY-PCM5100/5101/5102 module, for a headphone jack.
// It gets its own three pins rather than sharing the codec's bus, so only one
// of the two DACs is ever clocked: selecting an output re-routes the single
// I2S peripheral between these pins and the ES8311's (see applyOutput()).
//
// Tie the module's SCK to GND. That makes it run its internal PLL off BCK and
// want no master clock at all -- the ES8311 still needs the real MCLK on IO4
// and still has it. Floating SCK is the usual reason one of these is silent.
//
// The module's XSMT must be tied to 3V3. It is the DAC's soft mute and low
// means muted always -- this board doesn't fit its own pull-up, so without
// that wire nothing plays however correct the rest is. It only needs tying,
// not driving: cutting BCK/LRCK is what mutes this DAC when switching away
// (the PCM510x detects clock loss and mutes itself), which is why the pin
// below stays -1. Point it at a GPIO only for a second, explicit mute.
#define PIN_DAC_BCLK    2
#define PIN_DAC_LRCK    3       // also a strapping pin (JTAG select) -- see README
#define PIN_DAC_DOUT    14      // ESP32 -> DAC; this is the module's DIN
#define PIN_DAC_XSMT    -1      // soft mute, optional

// microSD, 4-bit SDIO
#define PIN_SD_CLK      38
#define PIN_SD_CMD      40
#define PIN_SD_D0       39
#define PIN_SD_D1       41
#define PIN_SD_D2       48
#define PIN_SD_D3       47

// Misc
#define PIN_RGB_LED     42      // single WS2812B
#define PIN_BAT_ADC     9
#define PIN_BOOT_KEY    0
