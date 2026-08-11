#include "ui.h"
#include "player.h"
#include "squirt.h"
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Layout notes
//
// The screen is 240x320. The previous version positioned every widget with
// hardcoded pixel offsets, which overflowed the bottom of the screen and put
// the transport buttons on top of the volume slider. This version uses a flex
// column so LVGL computes the vertical stack, and nothing can overlap however
// the font metrics change.
// ---------------------------------------------------------------------------

namespace {

constexpr lv_coord_t kArtPanelSize = 104;   // shared by the panel and updateArt()'s zoom math

lv_obj_t *scrBrowser  = nullptr;
lv_obj_t *scrPlaying  = nullptr;

lv_obj_t *lblPath     = nullptr;
lv_obj_t *listFiles   = nullptr;

lv_obj_t *lblTitle    = nullptr;
lv_obj_t *lblArtist   = nullptr;
lv_obj_t *lblElapsed  = nullptr;
lv_obj_t *lblTotal    = nullptr;
lv_obj_t *sldProgress = nullptr;
lv_obj_t *btnPlay     = nullptr;
lv_obj_t *lblPlayIcon = nullptr;
lv_obj_t *sldVolume   = nullptr;
lv_obj_t *lblBattery  = nullptr;
lv_obj_t *btnShuffle  = nullptr;
lv_obj_t *lblRepeat   = nullptr;
lv_obj_t *lblOutput   = nullptr;   // "SPK" / "HP" -- which DAC is unmuted

lv_obj_t *artIcon     = nullptr;   // placeholder, shown when the track has no art
lv_obj_t *artImg      = nullptr;   // decoded album art, shown in its place when there is one

lv_obj_t *scrSquirt    = nullptr;
lv_obj_t *listPeers    = nullptr;
lv_obj_t *swSquirtOn   = nullptr;
lv_obj_t *lblSquirtMsg = nullptr;
lv_obj_t *barSquirt    = nullptr;
lv_obj_t *lblSquirtHint = nullptr;

// The two prompts a transfer can't proceed without. Kept as pointers so they
// can also be torn down from the other side -- a peer walking out of range
// has to take its own dialog off this screen.
lv_obj_t *mbVerify = nullptr;
lv_obj_t *mbOffer  = nullptr;

std::vector<squirt::Peer> curPeers;

std::string        curDir = "/";
std::vector<Track> curEntries;
bool               draggingProgress = false;

// Owned by the UI task from the moment pickupArt() hands it over -- LVGL
// holds a pointer into curArtBytes via curArtDsc for as long as artImg
// displays it, so this must outlive that, not just the call that set it.
std::vector<uint8_t> curArtBytes;
lv_img_dsc_t          curArtDsc;
uint32_t              artGenSeen = 0;

// The panel-sized RGB565 copy actually handed to LVGL for JPEG art, and its
// descriptor. See scaleArtToPanel() for why the raw bytes can't just be
// zoomed. Same lifetime requirement as curArtBytes: LVGL points into this.
std::vector<uint8_t> curArtScaled;
lv_img_dsc_t          curArtScaledDsc;

void formatTime(uint32_t seconds, char *out, size_t len) {
    snprintf(out, len, "%lu:%02lu",
             (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}

void refreshBrowser();
void onSquirtOpen(lv_event_t *);   // defined with the rest of the squirt screen, below

// --- small helpers --------------------------------------------------------

// A transparent, non-scrollable, zero-padding container. Used as a row so the
// flex layout stays predictable.
lv_obj_t *makeRow(lv_obj_t *parent, lv_coord_t height) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *makeIconButton(lv_obj_t *parent, const char *symbol, lv_coord_t size,
                         lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);
    return btn;
}

// --- browser callbacks ----------------------------------------------------

void onEntryClicked(lv_event_t *e) {
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= curEntries.size()) return;

    if (curEntries[idx].isDir) {
        curDir = curEntries[idx].path;
        refreshBrowser();
        return;
    }
    player::playFile(curEntries[idx].path);
    lv_scr_load_anim(scrPlaying, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void onUpClicked(lv_event_t *) {
    if (curDir == "/") return;
    size_t slash = curDir.find_last_of('/');
    curDir = (slash == 0 || slash == std::string::npos) ? "/" : curDir.substr(0, slash);
    refreshBrowser();
}

void onNowPlayingClicked(lv_event_t *) {
    lv_scr_load_anim(scrPlaying, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void refreshBrowser() {
    curEntries = player::listDir(curDir);

    lv_label_set_text(lblPath, curDir.c_str());
    lv_obj_clean(listFiles);

    for (size_t i = 0; i < curEntries.size(); ++i) {
        const char *icon = curEntries[i].isDir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO;
        lv_obj_t *btn = lv_list_add_btn(listFiles, icon, curEntries[i].name.c_str());
        lv_obj_add_event_cb(btn, onEntryClicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_set_style_pad_ver(btn, 10, 0);
    }
    if (curEntries.empty()) {
        lv_list_add_text(listFiles, player::cardOk() ? "Nothing playable here"
                                                     : "No SD card");
    }
}

// --- now-playing callbacks ------------------------------------------------

void onBackClicked(lv_event_t *) {
    lv_scr_load_anim(scrBrowser, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

void onPlayClicked(lv_event_t *) { player::togglePause(); }
void onNextClicked(lv_event_t *) { player::next(); }
void onPrevClicked(lv_event_t *) { player::previous(); }

void onVolumeChanged(lv_event_t *e) {
    player::setVolume((uint8_t)lv_slider_get_value(lv_event_get_target(e)));
}

void onProgressEvent(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        draggingProgress = true;
    } else if (code == LV_EVENT_RELEASED) {
        draggingProgress = false;
        player::seekPercent((uint8_t)lv_slider_get_value(lv_event_get_target(e)));
    }
}

void onOutputClicked(lv_event_t *) {
    PlayerState s = player::snapshot();
    player::setOutput(s.output == Output::Headphones ? Output::Speaker : Output::Headphones);
}

void onShuffleClicked(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    bool on = lv_obj_has_state(b, LV_STATE_CHECKED);
    player::setShuffle(on);
}

void onRepeatClicked(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);

    // Cycle Off -> All -> One -> Off.
    static RepeatMode mode = RepeatMode::Off;
    mode = (mode == RepeatMode::Off) ? RepeatMode::All
         : (mode == RepeatMode::All) ? RepeatMode::One
                                     : RepeatMode::Off;
    player::setRepeat(mode);

    lv_label_set_text(lblRepeat, mode == RepeatMode::One ? LV_SYMBOL_LOOP "1"
                                                         : LV_SYMBOL_LOOP);
    if (mode == RepeatMode::Off) lv_obj_clear_state(b, LV_STATE_CHECKED);
    else                         lv_obj_add_state(b, LV_STATE_CHECKED);
}

// --- construction ---------------------------------------------------------

void buildBrowser() {
    scrBrowser = lv_obj_create(nullptr);
    lv_obj_set_style_pad_all(scrBrowser, 0, 0);

    lv_obj_t *bar = lv_obj_create(scrBrowser);
    lv_obj_set_size(bar, LV_PCT(100), 40);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btnUp = lv_btn_create(bar);
    lv_obj_set_size(btnUp, 34, 30);
    lv_obj_align(btnUp, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_all(btnUp, 0, 0);
    lv_obj_add_event_cb(btnUp, onUpClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *upIcon = lv_label_create(btnUp);
    lv_label_set_text(upIcon, LV_SYMBOL_LEFT);
    lv_obj_center(upIcon);

    lblPath = lv_label_create(bar);
    lv_label_set_long_mode(lblPath, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lblPath, 150);
    lv_obj_align(lblPath, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t *btnNP = lv_btn_create(bar);
    lv_obj_set_size(btnNP, 34, 30);
    lv_obj_align(btnNP, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_pad_all(btnNP, 0, 0);
    lv_obj_add_event_cb(btnNP, onNowPlayingClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *npIcon = lv_label_create(btnNP);
    lv_label_set_text(npIcon, LV_SYMBOL_AUDIO);
    lv_obj_center(npIcon);

    listFiles = lv_list_create(scrBrowser);
    lv_obj_set_size(listFiles, LV_PCT(100), 320 - 40);
    lv_obj_align(listFiles, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(listFiles, 0, 0);
    lv_obj_set_style_radius(listFiles, 0, 0);
    lv_obj_set_style_pad_all(listFiles, 0, 0);
}

void buildNowPlaying() {
    scrPlaying = lv_obj_create(nullptr);

    // Flex column: LVGL stacks the rows and applies the gap, so the total
    // height is computed rather than guessed.
    lv_obj_set_flex_flow(scrPlaying, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scrPlaying, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scrPlaying, 8, 0);
    lv_obj_set_style_pad_row(scrPlaying, 6, 0);
    lv_obj_clear_flag(scrPlaying, LV_OBJ_FLAG_SCROLLABLE);

    // Row 1: back button and battery -------------------------------------
    lv_obj_t *top = makeRow(scrPlaying, 30);

    lv_obj_t *btnBack = lv_btn_create(top);
    lv_obj_set_size(btnBack, 34, 28);
    lv_obj_align(btnBack, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_all(btnBack, 0, 0);
    lv_obj_add_event_cb(btnBack, onBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backIcon = lv_label_create(btnBack);
    lv_label_set_text(backIcon, LV_SYMBOL_LEFT);
    lv_obj_center(backIcon);

    lv_obj_t *btnSquirt = lv_btn_create(top);
    lv_obj_set_size(btnSquirt, 34, 28);
    lv_obj_align(btnSquirt, LV_ALIGN_LEFT_MID, 40, 0);
    lv_obj_set_style_pad_all(btnSquirt, 0, 0);
    lv_obj_add_event_cb(btnSquirt, onSquirtOpen, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *squirtIcon = lv_label_create(btnSquirt);
    lv_label_set_text(squirtIcon, LV_SYMBOL_UPLOAD);
    lv_obj_center(squirtIcon);

    lv_obj_t *btnOutput = lv_btn_create(top);
    lv_obj_set_size(btnOutput, 40, 28);
    lv_obj_align(btnOutput, LV_ALIGN_LEFT_MID, 80, 0);
    lv_obj_set_style_pad_all(btnOutput, 0, 0);
    lv_obj_add_event_cb(btnOutput, onOutputClicked, LV_EVENT_CLICKED, nullptr);
    lblOutput = lv_label_create(btnOutput);
    lv_label_set_text(lblOutput, "SPK");
    lv_obj_center(lblOutput);

    lblBattery = lv_label_create(top);
    lv_label_set_text(lblBattery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(lblBattery, LV_ALIGN_RIGHT_MID, 0, 0);

    // Row 2: artwork, or a placeholder icon when the track has none -------
    lv_obj_t *art = lv_obj_create(scrPlaying);
    lv_obj_set_size(art, kArtPanelSize, kArtPanelSize);
    lv_obj_set_style_radius(art, 12, 0);
    lv_obj_set_style_pad_all(art, 0, 0);   // so updateArt()'s manual centering math (against kArtPanelSize) is exact
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);

    artIcon = lv_label_create(art);
    lv_label_set_text(artIcon, LV_SYMBOL_AUDIO);
    lv_obj_center(artIcon);

    artImg = lv_img_create(art);
    lv_obj_add_flag(artImg, LV_OBJ_FLAG_HIDDEN);   // shown once real art loads
    // Deliberately left at the default VIRTUAL size mode, where zoom never
    // touches obj->coords (confirmed against lv_img.c) -- updateArt() sets
    // this widget's size and position explicitly, by hand, every time. Tried
    // relying on LVGL's own auto-sizing for this (REAL mode + SIZE_CONTENT,
    // lv_obj_center()) across several rounds; observed on real hardware that
    // it resolves on a delayed, unpredictable timing (sometimes the size
    // from a *previous* track, never a size or offset matching any
    // calculation done here), so centering computed against it was never
    // reliable. This has no dependency on any of that.

    // Row 3: title and artist ---------------------------------------------
    lblTitle = lv_label_create(scrPlaying);
    lv_label_set_long_mode(lblTitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lblTitle, LV_PCT(100));
    lv_obj_set_style_text_align(lblTitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lblTitle, "Nothing playing");

    lblArtist = lv_label_create(scrPlaying);
    lv_label_set_long_mode(lblArtist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lblArtist, LV_PCT(100));
    lv_obj_set_style_text_align(lblArtist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(lblArtist, LV_OPA_60, 0);
    lv_label_set_text(lblArtist, "");

    // Row 4: seek slider ---------------------------------------------------
    sldProgress = lv_slider_create(scrPlaying);
    lv_obj_set_size(sldProgress, LV_PCT(96), 6);
    lv_slider_set_range(sldProgress, 0, 100);
    lv_obj_add_event_cb(sldProgress, onProgressEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(sldProgress, onProgressEvent, LV_EVENT_RELEASED, nullptr);

    // Row 5: elapsed and total --------------------------------------------
    lv_obj_t *times = makeRow(scrPlaying, 16);
    lblElapsed = lv_label_create(times);
    lv_label_set_text(lblElapsed, "0:00");
    lv_obj_align(lblElapsed, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_text_opa(lblElapsed, LV_OPA_60, 0);

    lblTotal = lv_label_create(times);
    lv_label_set_text(lblTotal, "0:00");
    lv_obj_align(lblTotal, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_text_opa(lblTotal, LV_OPA_60, 0);

    // Row 6: transport -----------------------------------------------------
    lv_obj_t *transport = makeRow(scrPlaying, 52);

    lv_obj_t *btnPrev = makeIconButton(transport, LV_SYMBOL_PREV, 42, onPrevClicked);
    lv_obj_align(btnPrev, LV_ALIGN_CENTER, -62, 0);

    btnPlay = makeIconButton(transport, LV_SYMBOL_PLAY, 52, onPlayClicked);
    lv_obj_align(btnPlay, LV_ALIGN_CENTER, 0, 0);
    lblPlayIcon = lv_obj_get_child(btnPlay, 0);

    lv_obj_t *btnNext = makeIconButton(transport, LV_SYMBOL_NEXT, 42, onNextClicked);
    lv_obj_align(btnNext, LV_ALIGN_CENTER, 62, 0);

    // Row 7: shuffle, volume, repeat --------------------------------------
    lv_obj_t *bottom = makeRow(scrPlaying, 34);

    btnShuffle = makeIconButton(bottom, LV_SYMBOL_SHUFFLE, 32, onShuffleClicked);
    lv_obj_add_flag(btnShuffle, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_align(btnShuffle, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *btnRepeat = makeIconButton(bottom, LV_SYMBOL_LOOP, 32, onRepeatClicked);
    lv_obj_add_flag(btnRepeat, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_align(btnRepeat, LV_ALIGN_RIGHT_MID, 0, 0);
    lblRepeat = lv_obj_get_child(btnRepeat, 0);

    sldVolume = lv_slider_create(bottom);
    lv_obj_set_size(sldVolume, 108, 6);
    lv_obj_align(sldVolume, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(sldVolume, 0, 100);
    lv_slider_set_value(sldVolume, player::snapshot().volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(sldVolume, onVolumeChanged, LV_EVENT_VALUE_CHANGED, nullptr);
}

// --- squirt ---------------------------------------------------------------

void onSquirtBack(lv_event_t *) {
    lv_scr_load_anim(scrPlaying, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

void onSquirtOpen(lv_event_t *) {
    // The radio comes up with the screen and goes back down when it closes:
    // there is no reason to be discoverable while nobody is looking at this.
    squirt::setEnabled(true);
    lv_scr_load_anim(scrSquirt, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void onSquirtRadioToggled(lv_event_t *e) {
    squirt::setEnabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

void onPeerClicked(lv_event_t *e) {
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= curPeers.size()) return;

    PlayerState s = player::snapshot();
    if (s.path.empty()) {
        lv_label_set_text(lblSquirtMsg, "Play something first");
        return;
    }
    if (!squirt::sendTo(curPeers[idx].mac, s.path)) {
        lv_label_set_text(lblSquirtMsg, "Busy - try again");
        return;
    }
    lv_label_set_text(lblSquirtMsg, "Connecting...");
}

void onVerifyDeleted(lv_event_t *) { mbVerify = nullptr; }
void onOfferDeleted(lv_event_t *)  { mbOffer  = nullptr; }

// Both of these delete the dialog from inside that dialog's own event
// callback, so they have to use the async close -- the plain one frees the
// object LVGL is still dispatching an event on.
void onVerifyAnswered(lv_event_t *e) {
    lv_obj_t *mb = lv_event_get_current_target(e);
    squirt::confirmCode(lv_msgbox_get_active_btn(mb) == 0);
    // Forget it now, not when the async delete lands -- otherwise updateSquirt()
    // can see a live pointer to an object already queued for deletion and close
    // it a second time.
    mbVerify = nullptr;
    lv_msgbox_close_async(mb);
}

void onOfferAnswered(lv_event_t *e) {
    lv_obj_t *mb = lv_event_get_current_target(e);
    squirt::respondToOffer(lv_msgbox_get_active_btn(mb) == 0);
    mbOffer = nullptr;   // same reason as above
    lv_msgbox_close_async(mb);
}

void refreshPeerList() {
    curPeers = squirt::peers();
    lv_obj_clean(listPeers);
    if (curPeers.empty()) {
        lv_obj_t *btn = lv_list_add_btn(listPeers, nullptr, "Looking for devices...");
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    for (size_t i = 0; i < curPeers.size(); i++) {
        lv_obj_t *btn = lv_list_add_btn(listPeers, LV_SYMBOL_UPLOAD, curPeers[i].name);
        lv_obj_add_event_cb(btn, onPeerClicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

// Drives everything the transfer needs from the user. Called from tick(), so
// it runs whatever screen is showing -- an incoming squirt raises its own
// dialog on the top layer rather than waiting for the user to go looking.
void updateSquirt() {
    static uint32_t lastPeerRefresh = 0;
    static char     lastCode[7]     = {0};

    squirt::Status st = squirt::status();

    if (st.state == squirt::State::Verifying) {
        char code[7];
        if (squirt::verificationCode(code) && strcmp(code, lastCode) != 0) {
            strcpy(lastCode, code);
            if (mbVerify) lv_msgbox_close(mbVerify);

            static const char *btns[] = {"Match", "No", ""};
            char body[128];
            snprintf(body, sizeof(body),
                     "%s should be showing\nthe same six digits:\n\n        %s\n\nDo they match?",
                     st.peerName[0] ? st.peerName : "The other device", code);
            mbVerify = lv_msgbox_create(nullptr, "Check the code", body, btns, false);
            lv_obj_center(mbVerify);
            lv_obj_add_event_cb(mbVerify, onVerifyAnswered, LV_EVENT_VALUE_CHANGED, nullptr);
            lv_obj_add_event_cb(mbVerify, onVerifyDeleted, LV_EVENT_DELETE, nullptr);
        }
    } else {
        lastCode[0] = '\0';
        // The state moved on without this user answering -- the peer cancelled,
        // or it timed out. Take the stale dialog away rather than leaving a
        // prompt on screen that no longer does anything.
        if (mbVerify) lv_msgbox_close(mbVerify);
    }

    if (st.state == squirt::State::Deciding) {
        squirt::Offer offer;
        if (!mbOffer && squirt::pendingOffer(offer)) {
            static const char *btns[] = {"Accept", "Decline", ""};
            char body[160];
            snprintf(body, sizeof(body), "%s wants to send:\n\n%s\n%u KB",
                     offer.from[0] ? offer.from : "A nearby device",
                     offer.filename, (unsigned)(offer.bytes / 1024));
            mbOffer = lv_msgbox_create(nullptr, "Incoming track", body, btns, false);
            lv_obj_center(mbOffer);
            lv_obj_add_event_cb(mbOffer, onOfferAnswered, LV_EVENT_VALUE_CHANGED, nullptr);
            lv_obj_add_event_cb(mbOffer, onOfferDeleted, LV_EVENT_DELETE, nullptr);
        }
    } else if (mbOffer) {
        lv_msgbox_close(mbOffer);
    }

    if (lv_scr_act() != scrSquirt) return;

    if (lv_obj_has_state(swSquirtOn, LV_STATE_CHECKED) != squirt::enabled()) {
        if (squirt::enabled()) lv_obj_add_state(swSquirtOn, LV_STATE_CHECKED);
        else                   lv_obj_clear_state(swSquirtOn, LV_STATE_CHECKED);
    }

    uint32_t now = millis();
    if (now - lastPeerRefresh > 1000) {
        lastPeerRefresh = now;
        if (squirt::enabled()) refreshPeerList();
        else { lv_obj_clean(listPeers); curPeers.clear(); }
    }

    bool moving = st.state == squirt::State::Sending || st.state == squirt::State::Receiving;
    if (moving && st.total) {
        lv_obj_clear_flag(barSquirt, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(barSquirt, (int32_t)((uint64_t)st.transferred * 100 / st.total), LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(barSquirt, LV_OBJ_FLAG_HIDDEN);
    }

    if (st.message[0]) lv_label_set_text(lblSquirtMsg, st.message);
    else if (st.state == squirt::State::Idle) lv_label_set_text(lblSquirtMsg, "Ready");
}

void buildSquirt() {
    scrSquirt = lv_obj_create(nullptr);
    lv_obj_set_flex_flow(scrSquirt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scrSquirt, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scrSquirt, 8, 0);
    lv_obj_set_style_pad_row(scrSquirt, 6, 0);
    lv_obj_clear_flag(scrSquirt, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = makeRow(scrSquirt, 30);

    lv_obj_t *btnBack = lv_btn_create(top);
    lv_obj_set_size(btnBack, 34, 28);
    lv_obj_align(btnBack, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_all(btnBack, 0, 0);
    lv_obj_add_event_cb(btnBack, onSquirtBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backIcon = lv_label_create(btnBack);
    lv_label_set_text(backIcon, LV_SYMBOL_LEFT);
    lv_obj_center(backIcon);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "Squirt");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    swSquirtOn = lv_switch_create(top);
    lv_obj_set_size(swSquirtOn, 40, 22);
    lv_obj_align(swSquirtOn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(swSquirtOn, onSquirtRadioToggled, LV_EVENT_VALUE_CHANGED, nullptr);

    lblSquirtHint = lv_label_create(scrSquirt);
    lv_label_set_long_mode(lblSquirtHint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lblSquirtHint, LV_PCT(100));
    lv_obj_set_style_text_align(lblSquirtHint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(lblSquirtHint, LV_OPA_60, 0);
    lv_label_set_text(lblSquirtHint, "Tap a device to send the playing track.");

    listPeers = lv_list_create(scrSquirt);
    lv_obj_set_width(listPeers, LV_PCT(100));
    lv_obj_set_flex_grow(listPeers, 1);

    barSquirt = lv_bar_create(scrSquirt);
    lv_obj_set_size(barSquirt, LV_PCT(100), 6);
    lv_bar_set_range(barSquirt, 0, 100);
    lv_obj_add_flag(barSquirt, LV_OBJ_FLAG_HIDDEN);

    lblSquirtMsg = lv_label_create(scrSquirt);
    lv_label_set_long_mode(lblSquirtMsg, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lblSquirtMsg, LV_PCT(100));
    lv_obj_set_style_text_align(lblSquirtMsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lblSquirtMsg, "Off");
}

} // namespace

namespace ui {

void begin() {
    buildBrowser();
    buildNowPlaying();
    buildSquirt();
    refreshBrowser();
    lv_scr_load(scrBrowser);
}

// Diagnostic only: identifies which JPEG encoding variant a blob is, since
// TJpgDec (what lv_sjpg wraps) only supports baseline (SOF0) -- a decode
// failure on otherwise-valid-looking JPEG bytes almost always means
// progressive (SOF2) or another variant it can't handle at all.
void logJpegSofMarker(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
        Serial.println("[art] UI: not a JPEG (no FFD8 start-of-image)");
        return;
    }
    size_t i = 2;
    while (i + 4 <= bytes.size()) {
        if (bytes[i] != 0xFF) { i++; continue; }
        uint8_t marker = bytes[i + 1];
        // Markers with no length field.
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            const char *type = "unrecognized SOF variant";
            switch (marker) {
                case 0xC0: type = "baseline DCT (SOF0) -- should be supported"; break;
                case 0xC1: type = "extended sequential (SOF1) -- not supported by TJpgDec"; break;
                case 0xC2: type = "progressive DCT (SOF2) -- not supported by TJpgDec"; break;
                case 0xC3: type = "lossless (SOF3) -- not supported by TJpgDec"; break;
                default: break;
            }
            Serial.printf("[art] UI: JPEG marker 0xFF%02X -> %s\n", marker, type);
            // SOF0 says "baseline" but TJpgDec (tjpgd.c) still rejects some real
            // baseline files with JDR_FMT3 if the component count or chroma
            // subsampling isn't one it handles -- decode the SOF0 payload by hand
            // here (independent of tjpgd's own indexing) so a real rc=8 failure
            // is diagnosable from this log line alone instead of another round.
            if (marker == 0xC0) {
                size_t p = i + 4;   // payload starts right after the 2-byte length
                if (p + 6 <= bytes.size()) {
                    uint8_t  precision = bytes[p];
                    uint16_t h         = (bytes[p + 1] << 8) | bytes[p + 2];
                    uint16_t w         = (bytes[p + 3] << 8) | bytes[p + 4];
                    uint8_t  ncomp     = bytes[p + 5];
                    Serial.printf("[art] UI: SOF0 precision=%u w=%u h=%u ncomp=%u\n",
                                  precision, w, h, ncomp);
                    size_t  cp = p + 6;
                    uint8_t sampleOf[3] = {0, 0, 0};
                    for (uint8_t c = 0; c < ncomp && cp + 3 <= bytes.size(); c++, cp += 3) {
                        uint8_t id     = bytes[cp];
                        uint8_t sample = bytes[cp + 1];
                        uint8_t hs     = sample >> 4, vs = sample & 0x0F;
                        uint8_t qtid   = bytes[cp + 2];
                        if (c < 3) sampleOf[c] = sample;
                        Serial.printf("[art] UI:   component %u: id=%u sampling=0x%02X (H=%u V=%u) qtable=%u\n",
                                      c, id, sample, hs, vs, qtid);
                    }
                    if (ncomp != 1 && ncomp != 3) {
                        Serial.println("[art] UI:   -> TJpgDec only supports ncomp==1 or 3; this is why rc=8");
                    } else if (ncomp == 3) {
                        bool yOk  = sampleOf[0] == 0x11 || sampleOf[0] == 0x22 || sampleOf[0] == 0x21;
                        bool crOk = sampleOf[1] == 0x11 && sampleOf[2] == 0x11;
                        if (!yOk) {
                            Serial.println("[art] UI:   -> Y sampling isn't 4:4:4/4:2:0/4:2:2; TJpgDec rejects this, this is why rc=8");
                        } else if (!crOk) {
                            Serial.println("[art] UI:   -> Cb/Cr sampling isn't 1:1; TJpgDec rejects this, this is why rc=8");
                        }
                    }
                }
            }
            return;
        }
        uint16_t segLen = (bytes[i + 2] << 8) | bytes[i + 3];
        if (segLen < 2) break;   // malformed; avoid looping forever
        i += 2 + segLen;
    }
    Serial.println("[art] UI: ran out of data before finding a SOF marker");
}

// Decodes `raw` and resamples it down into curArtScaled at panel size, so the
// image LVGL draws is already the right size and needs no scaling at draw time.
//
// This exists because lv_img_set_zoom() provably cannot scale our art. LVGL
// only applies draw_dsc->zoom on the branch of lv_draw_img.c's
// decode_and_draw() taken when a decoder hands back a fully decoded buffer
// (img_data != NULL). When img_data is NULL it falls through to the
// read-line-by-line branch, which blits each source row 1:1 into a 1px-high
// area and never consults zoom at all. And lv_sjpg *always* sets
// img_data = NULL for in-memory JPEG (lv_sjpg.c's decoder_open, the
// LV_IMG_SRC_VARIABLE + is_jpg case) -- which is every piece of embedded art
// here. So the scaling has to happen before LVGL ever sees the image.
//
// Returns false if the source decodes to a full buffer after all (lv_png does,
// and zoom genuinely works for those) or if decoding fails, in which case the
// caller falls back to handing LVGL the raw bytes.
bool scaleArtToPanel(lv_img_dsc_t *raw, const lv_img_header_t &header) {
    lv_img_decoder_dsc_t d;
    if (lv_img_decoder_open(&d, raw, lv_color_black(), 0) != LV_RES_OK) {
        Serial.println("[art] UI: lv_img_decoder_open failed");
        return false;
    }
    if (d.img_data) {
        Serial.println("[art] UI: decoder gave a full buffer -- letting LVGL zoom it");
        lv_img_decoder_close(&d);
        return false;
    }

    // Fit the longest edge to the panel, preserving aspect ratio.
    uint16_t   longest = header.w > header.h ? header.w : header.h;
    lv_coord_t outW    = (lv_coord_t)(((uint32_t)header.w * kArtPanelSize) / longest);
    lv_coord_t outH    = (lv_coord_t)(((uint32_t)header.h * kArtPanelSize) / longest);
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;

    curArtScaled.assign((size_t)outW * outH * sizeof(lv_color_t), 0);
    // LVGL sizes its own read_line buffer this way -- the extra byte per pixel
    // covers formats that carry alpha, which this one doesn't but might later.
    std::vector<uint8_t> line((size_t)header.w * LV_IMG_PX_SIZE_ALPHA_BYTE);

    bool ok = true;
    for (lv_coord_t oy = 0; oy < outH; oy++) {
        lv_coord_t sy = (lv_coord_t)(((uint32_t)oy * header.h) / outH);
        if (lv_img_decoder_read_line(&d, 0, sy, header.w, line.data()) != LV_RES_OK) {
            Serial.printf("[art] UI: read_line failed at source row %d\n", (int)sy);
            ok = false;
            break;
        }
        const lv_color_t *srcPx = (const lv_color_t *)line.data();
        lv_color_t       *dstPx = (lv_color_t *)curArtScaled.data() + (size_t)oy * outW;
        for (lv_coord_t ox = 0; ox < outW; ox++) {
            dstPx[ox] = srcPx[((uint32_t)ox * header.w) / outW];
        }
    }
    lv_img_decoder_close(&d);
    if (!ok) return false;

    curArtScaledDsc.header.always_zero = 0;
    curArtScaledDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    curArtScaledDsc.header.w  = outW;
    curArtScaledDsc.header.h  = outH;
    curArtScaledDsc.data_size = curArtScaled.size();
    curArtScaledDsc.data      = curArtScaled.data();
    Serial.printf("[art] UI: pre-scaled %ux%u -> %dx%d (%u bytes)\n",
                  (unsigned)header.w, (unsigned)header.h, (int)outW, (int)outH,
                  (unsigned)curArtScaled.size());
    return true;
}

// Checks for new album art (at most once per track, see pickupArt()) and
// swaps the placeholder icon for the decoded image, or back, accordingly.
// curArtBytes has to outlive the lv_img displaying it -- LVGL holds a
// pointer into it via curArtDsc, it doesn't copy -- so this only replaces
// that buffer once the old image is no longer the one on screen.
void updateArt() {
    std::vector<uint8_t> newArt;
    if (!player::pickupArt(artGenSeen, newArt)) return;

    Serial.printf("[art] UI: updateArt, %u bytes picked up\n", (unsigned)newArt.size());
    curArtBytes = std::move(newArt);

    if (curArtBytes.empty()) {
        Serial.println("[art] UI: empty -- showing placeholder");
        lv_obj_add_flag(artImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(artIcon, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    curArtDsc.header.always_zero = 0;
    curArtDsc.header.w  = 0;
    curArtDsc.header.h  = 0;
    curArtDsc.header.cf = LV_IMG_CF_RAW;
    curArtDsc.data_size = curArtBytes.size();
    curArtDsc.data      = curArtBytes.data();

    Serial.printf("[art] UI: first 4 bytes = %02X %02X %02X %02X\n",
                  curArtBytes[0], curArtBytes[1], curArtBytes[2], curArtBytes[3]);

    lv_img_header_t header;
    lv_res_t res = lv_img_decoder_get_info(&curArtDsc, &header);
    Serial.printf("[art] UI: lv_img_decoder_get_info res=%d w=%u h=%u cf=%u\n",
                  (int)res, (unsigned)header.w, (unsigned)header.h, (unsigned)header.cf);

    if (res != LV_RES_OK || !header.w || !header.h) {
        // Not a JPEG/PNG we can decode (unsupported format or corrupt data)
        // -- fall back to the placeholder rather than show nothing/garbage.
        Serial.println("[art] UI: decode info failed -- showing placeholder");
        logJpegSofMarker(curArtBytes);
        lv_obj_add_flag(artImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(artIcon, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Both descriptors live at fixed addresses and get new contents every
    // track, but LVGL's image cache is keyed on the source *pointer* -- without
    // this it can serve the previous track's decoded image for the new one.
    lv_img_cache_invalidate_src(&curArtDsc);
    lv_img_cache_invalidate_src(&curArtScaledDsc);

    if (scaleArtToPanel(&curArtDsc, header)) {
        // Already panel-sized: no zoom, and the geometry is exactly known.
        lv_img_set_src(artImg, &curArtScaledDsc);
        lv_img_set_zoom(artImg, LV_IMG_ZOOM_NONE);
        lv_coord_t w = curArtScaledDsc.header.w;
        lv_coord_t h = curArtScaledDsc.header.h;
        lv_obj_set_size(artImg, w, h);
        lv_obj_set_pos(artImg, (kArtPanelSize - w) / 2, (kArtPanelSize - h) / 2);
        lv_obj_update_layout(artImg);

        lv_obj_clear_flag(artImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(artIcon, LV_OBJ_FLAG_HIDDEN);
        Serial.printf("[art] UI: showing pre-scaled art, artImg coords x=%d y=%d w=%d h=%d\n",
                      (int)lv_obj_get_x(artImg), (int)lv_obj_get_y(artImg),
                      (int)lv_obj_get_width(artImg), (int)lv_obj_get_height(artImg));
        return;
    }

    lv_img_set_src(artImg, &curArtDsc);

    // Fit the panel regardless of the source image's native size -- embedded
    // art is rarely pre-sized for this (unless it's been through
    // tools/compress_art.py). Size and position are computed and set here by
    // hand rather than via lv_img_set_zoom() + lv_obj_center() with LVGL's
    // own zoom-aware auto-sizing (REAL size mode + SIZE_CONTENT): tried that
    // across several rounds and confirmed on real hardware that it resolves
    // on unpredictable timing -- observed sizes belonging to a *previous*
    // track, never matching what the current call should have produced, no
    // matter what was done to try to force it to resolve immediately. Manual
    // geometry has no such dependency.
    uint16_t longest = header.w > header.h ? header.w : header.h;
    uint16_t zoom = (uint16_t)((kArtPanelSize * 256) / longest);
    lv_img_set_zoom(artImg, zoom);
    lv_img_set_antialias(artImg, true);

    lv_coord_t zoomedW = (lv_coord_t)(((uint32_t)header.w * zoom) / 256);
    lv_coord_t zoomedH = (lv_coord_t)(((uint32_t)header.h * zoom) / 256);
    lv_obj_set_size(artImg, zoomedW, zoomedH);
    lv_obj_set_pos(artImg, (kArtPanelSize - zoomedW) / 2, (kArtPanelSize - zoomedH) / 2);
    // lv_obj_set_size()/set_pos() only set style props and mark the object's
    // layout dirty (lv_obj_mark_layout_as_dirty(), lv_obj_pos.c) -- obj->coords
    // itself isn't touched until something walks that dirty list and calls
    // lv_obj_refr_size()/refr_pos() (lv_obj_pos.c's layout_update_core()).
    // Every previous attempt here read back geometry (or relied on layout
    // implicitly happening in time) before that resolution ever ran, which is
    // why they all showed stale/native coords. This forces it synchronously.
    lv_obj_update_layout(artImg);

    lv_obj_clear_flag(artImg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(artIcon, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[art] UI: showing art, zoom=%u, artImg hidden=%d\n",
                  (unsigned)zoom, (int)lv_obj_has_flag(artImg, LV_OBJ_FLAG_HIDDEN));
    Serial.printf("[art] UI: artImg coords x=%d y=%d w=%d h=%d, parent 'art' x=%d y=%d w=%d h=%d\n",
                  (int)lv_obj_get_x(artImg), (int)lv_obj_get_y(artImg),
                  (int)lv_obj_get_width(artImg), (int)lv_obj_get_height(artImg),
                  (int)lv_obj_get_x(lv_obj_get_parent(artImg)), (int)lv_obj_get_y(lv_obj_get_parent(artImg)),
                  (int)lv_obj_get_width(lv_obj_get_parent(artImg)), (int)lv_obj_get_height(lv_obj_get_parent(artImg)));
}

void tick() {
    static uint32_t last = 0;
    if (millis() - last < 250) return;
    last = millis();

    // Before the early-out below: a squirt can arrive while the user is on any
    // screen, and its prompts are what stop a transfer happening unattended.
    updateSquirt();

    // Only the now-playing screen needs refreshing.
    if (lv_scr_act() != scrPlaying) return;

    updateArt();

    // One locked read for everything, so the UI never blocks on the decoder.
    PlayerState s = player::snapshot();

    lv_label_set_text(lblTitle,  s.title.c_str());
    lv_label_set_text(lblArtist, s.artist.c_str());
    lv_label_set_text(lblOutput, s.output == Output::Headphones ? "HP" : "SPK");

    char buf[16];
    formatTime(s.elapsed, buf, sizeof(buf));
    lv_label_set_text(lblElapsed, buf);
    formatTime(s.duration, buf, sizeof(buf));
    lv_label_set_text(lblTotal, buf);

    if (!draggingProgress && s.duration > 0) {
        lv_slider_set_value(sldProgress, (s.elapsed * 100) / s.duration, LV_ANIM_OFF);
    }

    lv_label_set_text(lblPlayIcon,
                      (s.playing && !s.paused) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    float v = player::batteryVolts();
    lv_label_set_text(lblBattery,
        v > 4.0f ? LV_SYMBOL_BATTERY_FULL  :
        v > 3.8f ? LV_SYMBOL_BATTERY_3     :
        v > 3.6f ? LV_SYMBOL_BATTERY_2     :
        v > 3.4f ? LV_SYMBOL_BATTERY_1     : LV_SYMBOL_BATTERY_EMPTY);
}

} // namespace ui
