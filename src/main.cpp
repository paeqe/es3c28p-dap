#include <Arduino.h>
#include <lvgl.h>
#include <Wire.h>
#include <esp_timer.h>

#include "board_config.h"
#include "display.h"
#include "touch.h"
#include "player.h"
#include "squirt.h"
#include "tonetest.h"
#include "ui.h"

static LGFX tft;

static constexpr uint32_t BUF_LINES = 40;
static lv_disp_draw_buf_t drawBuf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

static void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.writePixels((lgfx::rgb565_t *)px, w * h);
    tft.endWrite();

    lv_disp_flush_ready(drv);
}

static void touchCb(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x, y;
    if (touch::read(x, y)) {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvglTick(void *) { lv_tick_inc(1); }

// Shown briefly at boot so the board reports its own state without needing
// a serial connection.
static void showDiagnostics(bool touchOk) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *lbl = lv_label_create(scr);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Boot report\n\n"
             "Touch:  %s\n"
             "Codec:  %s\n"
             "Card:   %s\n"
             "PSRAM:  %s\n\n"
             "%s",
             touchOk              ? "ok" : "FAILED",
             player::codecOk()    ? "ok" : "FAILED",
             player::cardOk()     ? "ok" : "FAILED",
             psramFound()         ? "ok" : "absent",
             (player::codecOk() && player::cardOk())
                 ? "Starting..." : "Continuing in degraded mode");

    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_center(lbl);

    uint32_t until = millis() + 4000;
    while (millis() < until) { lv_timer_handler(); delay(5); }
}

// Draws a marker wherever a touch is detected and prints raw values, so the
// axis transform can be worked out by eye. Hold BOOT to skip.
static void touchTestScreen() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("TOUCH TEST", 10, 10);
    tft.drawString("Tap each corner marker.", 10, 25);
    tft.drawString("Dot should land under finger.", 10, 38);
    tft.drawString("Hold BOOT to continue.", 10, 51);

    // Corner targets, 12px in from each edge.
    const int32_t tx[4] = {12, LCD_WIDTH - 13, 12, LCD_WIDTH - 13};
    const int32_t ty[4] = {80, 80, LCD_HEIGHT - 13, LCD_HEIGHT - 13};
    for (int i = 0; i < 4; i++) {
        tft.drawCircle(tx[i], ty[i], 10, TFT_DARKGREY);
        tft.drawPixel(tx[i], ty[i], TFT_WHITE);
    }

    pinMode(PIN_BOOT_KEY, INPUT_PULLUP);
    uint32_t lastPrint = 0;

    while (digitalRead(PIN_BOOT_KEY) == HIGH) {
        int32_t rx, ry;
        uint8_t fingers;

        if (touch::readRaw(rx, ry, fingers)) {
            int32_t sx, sy;
            touch::read(sx, sy);
            tft.fillCircle(sx, sy, 4, TFT_GREEN);

            if (millis() - lastPrint > 150) {
                lastPrint = millis();
                Serial.printf("[touch] raw x=%3ld y=%3ld  ->  screen x=%3ld y=%3ld  (%u finger)\n",
                              (long)rx, (long)ry, (long)sx, (long)sy, fingers);
                tft.fillRect(0, 62, LCD_WIDTH, 12, TFT_BLACK);
                char buf[48];
                snprintf(buf, sizeof(buf), "raw %ld,%ld", (long)rx, (long)ry);
                tft.drawString(buf, 10, 62);
            }
        }
        delay(10);
    }
    tft.fillScreen(TFT_BLACK);
    delay(300);
}

// USB CDC drops anything printed before the host attaches, and the host
// reattaches after every reset. So the boot summary is stored and reprinted
// periodically for the first half minute, which means it is caught whenever
// the monitor happens to connect.
static String gBootReport;

static void reprintBootReport() {
    static uint32_t last = 0;
    static uint8_t  times = 0;
    if (times >= 10) return;
    if (millis() - last < 3000) return;
    last = millis();
    times++;
    Serial.print(gBootReport);
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(10);
    delay(300);
    Serial.println("\n[boot] ES3C28P DAP");
    Serial.printf("[boot] PSRAM: %s (%u bytes free)\n",
                  psramFound() ? "found" : "ABSENT", ESP.getFreePsram());

    // One I2C owner for both the touch controller and the codec.
    Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
    touch::scanBus();

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(160);
    tft.fillScreen(TFT_BLACK);

    bool touchOk = touch::begin();

    // Both axes are mirrored on this panel relative to the controller's
    // native orientation.
    touch::setTransform(false, true, true);

    if (touchOk) touchTestScreen();

    lv_init();

    size_t bufPixels = LCD_WIDTH * BUF_LINES;
    buf1 = (lv_color_t *)heap_caps_malloc(bufPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    buf2 = (lv_color_t *)heap_caps_malloc(bufPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!buf1) {
        Serial.println("[boot] no PSRAM buffer, falling back to internal RAM");
        bufPixels = LCD_WIDTH * 16;
        buf1 = (lv_color_t *)heap_caps_malloc(bufPixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
        buf2 = nullptr;
    }
    lv_disp_draw_buf_init(&drawBuf, buf1, buf2, bufPixels);

    static lv_disp_drv_t dispDrv;
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res  = LCD_WIDTH;
    dispDrv.ver_res  = LCD_HEIGHT;
    dispDrv.flush_cb = flushCb;
    dispDrv.draw_buf = &drawBuf;
    lv_disp_drv_register(&dispDrv);

    static lv_indev_drv_t indevDrv;
    lv_indev_drv_init(&indevDrv);
    indevDrv.type    = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchCb;
    lv_indev_drv_register(&indevDrv);

    const esp_timer_create_args_t targs = {
        .callback = &lvglTick, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "lv_tick",
        .skip_unhandled_events = true
    };
    esp_timer_handle_t th;
    esp_timer_create(&targs, &th);
    esp_timer_start_periodic(th, 1000);

    // Tone test: hold BOOT while resetting to hear a scale and sweep straight
    // out of the codec, with no SD card or decoder involved.
    pinMode(PIN_BOOT_KEY, INPUT_PULLUP);
    if (digitalRead(PIN_BOOT_KEY) == LOW) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("TONE TEST", 10, 10);
        tft.drawString("Listen to the speaker.", 10, 26);

        if (player::initCodec()) {
            tft.drawString("Codec ok - playing...", 10, 42);
            tonetest::run();
            tft.drawString("Done.", 10, 58);
        } else {
            tft.drawString("Codec FAILED - no I2C.", 10, 42);
        }

        // Then the same tone at the external DAC, on its own I2S driver, with
        // none of the audio library or the runtime pin switching involved. If
        // the speaker played and this doesn't, it's the module or its wiring.
        tft.drawString("Now the headphone jack...", 10, 74);
        tonetest::runDac();
        tft.drawString("Done.", 10, 90);

        delay(1500);
        tft.fillScreen(TFT_BLACK);
    }

    // Failures here are no longer fatal - the UI still loads so the display
    // and touch can be verified independently of the card and the codec.
    player::begin();

    showDiagnostics(touchOk);

    gBootReport  = "\n===== BOOT REPORT =====\n";
    gBootReport += "Touch : "; gBootReport += touchOk ? "ok" : "FAILED"; gBootReport += "\n";
    gBootReport += "Codec : "; gBootReport += player::codecOk() ? "ok" : "FAILED"; gBootReport += "\n";
    gBootReport += "Card  : "; gBootReport += player::cardOk() ? "ok" : "FAILED"; gBootReport += "\n";
    gBootReport += "SD    : "; gBootReport += player::sdModeString(); gBootReport += "\n";
    gBootReport += "PSRAM : "; gBootReport += psramFound() ? "ok" : "ABSENT"; gBootReport += "\n";
    gBootReport += "=======================\n";
    Serial.print(gBootReport);

    // Prepares the task and queues but leaves the radio off -- squirt only goes
    // on air while its screen is open. See the security notes in squirt.cpp.
    squirt::begin();

    ui::begin();
    Serial.println("[boot] ready");
}

void loop() {
    lv_timer_handler();
    ui::tick();
    reprintBootReport();
    delay(5);
}
