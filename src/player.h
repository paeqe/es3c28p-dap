#pragma once
#include <Arduino.h>
#include <vector>
#include <string>

struct Track {
    std::string path;       // full path on the card
    std::string name;       // display name, extension stripped
    bool        isDir;
};

enum class RepeatMode { Off, All, One };

// Where sound comes out. Both destinations sit on the same I2S bus and hear
// the same stream, so selecting one really means silencing the other.
enum class Output {
    Speaker,      // ES8311 -> FM8002E -> speaker header
    Headphones,   // external GY-PCM5100/5101/5102 module's jack
};

// A consistent view of the player, copied out in one locked operation so the
// UI never sees half-updated state and never blocks behind the decoder.
struct PlayerState {
    bool        playing   = false;
    bool        paused    = false;
    uint32_t    elapsed   = 0;
    uint32_t    duration  = 0;
    uint8_t     volume    = 60;
    RepeatMode  repeat    = RepeatMode::Off;
    bool        shuffle   = false;
    Output      output    = Output::Speaker;
    std::string title;
    std::string artist;
    std::string path;       // full path of the playing file, empty if none
};

namespace player {

// Bring up SD, codec, amplifier and the audio task. Returns true only if both
// the card and the codec came up; query the two below to find out which failed.
bool begin();
bool codecOk();
bool cardOk();

// Which SD bus mode the card actually mounted at, e.g. "4-bit @ 40MHz".
const char *sdModeString();

// Configures the ES8311 and enables the amplifier, without touching I2S or
// the SD card. Called by begin(), but exposed separately so the tone test can
// run before the audio library installs its own I2S driver.
bool initCodec();

// --- browsing -------------------------------------------------------------
// Safe to call from the UI task. Directories come first, then playable files,
// both sorted alphabetically.
std::vector<Track> listDir(const std::string &path);
bool isPlayable(const std::string &filename);

// --- control --------------------------------------------------------------
// These post a command to the audio task and return immediately. None of them
// block on the decoder.
// Starts this exact file immediately, then builds the rest of the folder into
// the queue in the background. Pass the full path, e.g. "/Music/track.mp3".
void playFile(const std::string &fullPath);
void togglePause();
void next();
void previous();
void stop();
void seekPercent(uint8_t percent);
void setVolume(uint8_t percent);          // 0-100
void setRepeat(RepeatMode m);
void setShuffle(bool on);

// Switches between the onboard speaker and the external PCM510x jack. Both
// DACs are fed the same I2S stream, so this mutes one and unmutes the other,
// briefly silencing both in between so the change doesn't pop. It also moves
// volume control to whichever path can do it: the ES8311 attenuates in
// hardware, the PCM510x has no volume register at all (no I2C on that part),
// so on headphones the decoder scales the samples in software instead.
void setOutput(Output o);

// Prints where the I2S peripheral's signals are actually routed right now, by
// reading the GPIO matrix rather than trusting what the code asked for, plus
// the decoder/volume state. Runs automatically on every output switch; call it
// directly to inspect the current state at any time.
void diagnoseOutput();

// --- state ----------------------------------------------------------------
// One locked copy, held for microseconds. Call once per UI refresh.
PlayerState snapshot();

// --- album art --------------------------------------------------------
// Raw embedded image bytes (JPEG or PNG) for the current track, extracted
// from MP3 ID3 APIC frames or FLAC PICTURE metadata blocks. Other formats
// (WAV/M4A/AAC/OGG/Opus) never populate this.
//
// Pass in the generation you last saw (start at 0). Returns true and moves
// a new result into `out` at most once per track -- whether or not the
// track actually has art, so an empty `out` on a true return means "this
// track has none" and the caller should fall back to a placeholder.
// Ownership of the bytes transfers to the caller; nothing else touches this
// buffer again once picked up, so it's safe to hold and reference (e.g. from
// an lv_img source) for as long as needed.
bool pickupArt(uint32_t &lastSeenGeneration, std::vector<uint8_t> &out);

// --- card access for other subsystems -------------------------------------
// The decoder reads the card from the audio task, so anything else touching
// SD_MMC has to serialize against it or the reads interleave and corrupt each
// other. Take this around every SD_MMC call made from another task (squirt's
// file writes are the only current user). Recursive, like the internal uses.
// Returns false if the lock could not be taken within timeoutMs -- callers
// must handle that rather than proceeding, since it means the audio task is
// busy or wedged, and must not call unlockCard() in that case.
bool lockCard(uint32_t timeoutMs);
void unlockCard();

// Battery voltage in volts, read through the divider on IO9.
float batteryVolts();

} // namespace player
