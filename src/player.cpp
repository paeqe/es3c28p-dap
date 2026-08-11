#include "player.h"
#include "es8311.h"
#include "board_config.h"

#include <Audio.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <algorithm>
#include <utility>
#include <cstring>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <soc/gpio_reg.h>
#include <soc/gpio_sig_map.h>

// ---------------------------------------------------------------------------
// Threading model
//
// The audio task owns the Audio object and the playlist outright. Nothing else
// touches them. The UI communicates by posting commands to a queue, and reads
// state through a snapshot guarded by a mutex that is only ever held for a few
// microseconds. The previous design wrapped audio.loop() in the same mutex the
// UI polled, which starved the UI and froze the elapsed timer.
// ---------------------------------------------------------------------------

namespace {

enum class CmdType : uint8_t {
    PlayFile, TogglePause, Next, Prev, Stop,
    Seek, Volume, Repeat, Shuffle, SetOutput
};

struct Cmd {
    CmdType  type;
    uint32_t a = 0;
    char     path[192] = {0};
};

// A watchdog-triggered reset is RTC_SW_CPU_RST, which leaves RTC memory
// untouched (unlike a power cycle) -- so the audio task can leave a
// breadcrumb of exactly which step it was on right before a hang, and
// begin() can read it back on the very next boot. This exists because the
// task_wdt panic handler's own backtrace resolves to the idle task /
// panic-handling chain, not the hung task's actual PC, so it can't otherwise
// say where inside a blocking wait the task got stuck.
RTC_NOINIT_ATTR uint32_t gCheckpoint;
RTC_NOINIT_ATTR uint32_t gCheckpointMagic;
constexpr uint32_t kCheckpointMagic = 0xC0FFEE42;

const char *checkpointName(uint32_t cp) {
    switch (cp) {
        case 1:   return "loop: draining commands";
        case 2:   return "loop: before gSdLock (gAudio.loop wrapper)";
        case 3:   return "loop: inside gAudio.loop()";
        case 4:   return "loop: after gAudio.loop(), releasing gSdLock";
        case 5:   return "loop: scanStep()";
        case 6:   return "loop: publishProgress/isRunning check";
        case 100: return "seek: computing target";
        case 101: return "seek: before gSdLock (setFilePos)";
        case 102: return "seek: inside gAudio.setFilePos()";
        case 200: return "startPath: before gSdLock";
        case 201: return "startPath: inside gAudio.stopSong()";
        case 202: return "startPath: inside gAudio.connecttoFS()";
        default:  return "unknown";
    }
}

void checkpoint(uint32_t cp) {
    gCheckpoint = cp;
    gCheckpointMagic = kCheckpointMagic;
}

Audio        gAudio;
ES8311       gCodec;

QueueHandle_t     gCmdQueue  = nullptr;
SemaphoreHandle_t gStateLock = nullptr;

// Owned by the audio task only.
std::vector<Track>  gQueue;
std::vector<size_t> gOrder;
size_t              gPos     = 0;
RepeatMode          gRepeat  = RepeatMode::Off;
bool                gShuffle = false;
Output              gOutput  = Output::Speaker;

// Published state, guarded by gStateLock.
PlayerState gState;

bool gCodecOk = false;
bool gCardOk  = false;
const char *gSdMode = "not mounted";

// Serializes actual SD_MMC/FATFS calls. Almost everything below now avoids
// touching the card at all (see DirCache) -- this only still matters for the
// decoder's own reads in gAudio.loop(), the file open in startPath(), and the
// now-rare cache-miss fallback in listDir()/scanStep(). Recursive because
// audio_eof_mp3() calls back into startPath() *from inside* the locked
// gAudio.loop() call, on this same task.
SemaphoreHandle_t gSdLock = nullptr;

// Cache of recent directory listings (dirs+files, same shape listDir()
// returns), shared between the UI task's browsing and the audio task's queue
// build. Tapping a file always follows browsing into its folder, so the
// queue build below almost always hits this instead of touching the card --
// this is the "preload" the slow-load complaints were really asking for:
// not fetching audio data early, but never re-walking a directory twice.
struct DirCache {
    static constexpr size_t kCapacity = 6;
    struct Entry { std::string path; std::vector<Track> listing; };
    std::vector<Entry> entries;      // front = most recently used
    SemaphoreHandle_t  lock = nullptr;

    void init() { if (!lock) lock = xSemaphoreCreateMutex(); }

    bool get(const std::string &path, std::vector<Track> &out) {
        if (!lock) return false;
        xSemaphoreTake(lock, portMAX_DELAY);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].path == path) {
                out = entries[i].listing;
                if (i != 0) std::rotate(entries.begin(), entries.begin() + i, entries.begin() + i + 1);
                xSemaphoreGive(lock);
                return true;
            }
        }
        xSemaphoreGive(lock);
        return false;
    }

    void put(const std::string &path, std::vector<Track> listing) {
        if (!lock) return;
        xSemaphoreTake(lock, portMAX_DELAY);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].path == path) { entries.erase(entries.begin() + i); break; }
        }
        entries.insert(entries.begin(), {path, std::move(listing)});
        if (entries.size() > kCapacity) entries.pop_back();
        xSemaphoreGive(lock);
    }

};
DirCache gDirCache;

// Handoff for embedded album art: the audio task extracts raw JPEG/PNG bytes
// (or decides a track has none) and hands them off here; the UI task picks
// them up via generation number and takes over ownership of the buffer from
// that point on, so there is no ongoing shared access to synchronize once
// pickupArt() returns true.
struct PendingArt {
    std::vector<uint8_t> bytes;
    uint32_t             generation = 0;
};
PendingArt        gPendingArt;
SemaphoreHandle_t gArtLock = nullptr;

// Runs on the audio task. Empty bytes means "this track has no art" -- still
// bumps the generation, so the UI correctly clears any art left over from
// the previous track instead of leaving it showing.
void setPendingArt(std::vector<uint8_t> &&bytes) {
    if (!gArtLock) return;
    size_t n = bytes.size();
    xSemaphoreTake(gArtLock, portMAX_DELAY);
    gPendingArt.bytes = std::move(bytes);
    gPendingArt.generation++;
    uint32_t gen = gPendingArt.generation;
    xSemaphoreGive(gArtLock);
    Serial.printf("[art] setPendingArt: %u bytes, generation now %u\n", (unsigned)n, (unsigned)gen);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string stripExtension(const std::string &name) {
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

void publish(bool playing, bool paused) {
    if (!gStateLock) return;
    xSemaphoreTake(gStateLock, portMAX_DELAY);
    gState.playing  = playing;
    gState.paused   = paused;
    gState.repeat   = gRepeat;
    gState.shuffle  = gShuffle;
    xSemaphoreGive(gStateLock);
}

void publishTitle(const std::string &title, const std::string &artist) {
    if (!gStateLock) return;
    xSemaphoreTake(gStateLock, portMAX_DELAY);
    gState.title  = title;
    gState.artist = artist;
    xSemaphoreGive(gStateLock);
}

// Kept separate from publishTitle() because the title is published optimistically
// at the top of startPath(), before the file is known to open -- the path is only
// published once it actually did, so nothing (squirt especially) can pick up a
// path that isn't really playing.
void publishPath(const std::string &path) {
    if (!gStateLock) return;
    xSemaphoreTake(gStateLock, portMAX_DELAY);
    gState.path = path;
    xSemaphoreGive(gStateLock);
}

void publishProgress() {
    if (!gStateLock) return;
    uint32_t e = gAudio.getAudioCurrentTime();
    uint32_t d = gAudio.getAudioFileDuration();
    xSemaphoreTake(gStateLock, portMAX_DELAY);
    gState.elapsed  = e;
    gState.duration = d;
    xSemaphoreGive(gStateLock);
}

// Applies a 0-100 volume to whichever output is live.
//
// The two paths attenuate in completely different places. The ES8311 has a
// real volume register, so the decoder runs at full scale and the codec does
// the work. The PCM510x deliberately doesn't: the A-suffix parts in these
// modules have no I2C at all, so there is nothing to write a volume to, and
// the only place left to do it is in the sample stream itself. Hence the
// library's own 0-21 software volume on that path, and full scale on the
// other -- applying both would attenuate twice and waste bit depth.
void applyVolume(uint8_t pct) {
    if (pct > 100) pct = 100;
    if (gOutput == Output::Headphones) {
        gCodec.setVolume(0);
        gAudio.setVolume((uint8_t)((pct * 21 + 50) / 100));
    } else {
        gAudio.setVolume(21);
        gCodec.setVolume(pct);
    }
}

// Detaches a pad from the I2S peripheral and parks it low.
//
// This is not optional. i2s_set_pin() -- which is all Audio::setPinout() does
// -- only ever *adds* a route through the GPIO matrix; it never removes the
// previous one. Without this, switching output would leave the DAC we just
// left still receiving clock and data alongside the new one, and both would
// play at once.
void releaseI2SPin(int pin) {
    if (pin < 0) return;
    gpio_reset_pin((gpio_num_t)pin);   // returns the pad to plain GPIO
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

// Reports what a pad is *actually* driven by, straight out of the GPIO matrix,
// rather than what the code believes it asked for. This is the one thing that
// can tell an "I called setPinout and it silently didn't take" fault apart from
// a wiring or module fault, so it reads the hardware and nothing else.
void reportPin(const char *label, int pin, uint32_t expectSig) {
    if (pin < 0) {
        Serial.printf("[audio]   %-5s : not assigned\n", label);
        return;
    }
    uint32_t sel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + (pin * 4)) & 0x1FF;
    bool     oen = pin < 32 ? (REG_READ(GPIO_ENABLE_REG) >> pin) & 1
                            : (REG_READ(GPIO_ENABLE1_REG) >> (pin - 32)) & 1;

    const char *verdict;
    if (sel == expectSig)              verdict = "I2S driving it";
    else if (sel == SIG_GPIO_OUT_IDX)  verdict = "plain GPIO (detached)";
    else                               verdict = "some other signal!";

    Serial.printf("[audio]   %-5s IO%-2d: out_sel=%3u expect=%3u out_en=%d  %s\n",
                  label, pin, (unsigned)sel, (unsigned)expectSig, (int)oen, verdict);
}

// There is one I2S peripheral and two DACs on separate pins, so choosing an
// output means pointing that peripheral at one set of pins and cutting the
// other loose. Cutting the clock is itself the mute for the PCM510x -- it
// detects clock loss and mutes -- which is why XSMT is optional. Everything
// is silent for a moment in between, because switching a live amp in one step
// is what makes the pop.
void applyOutput(Output o, uint8_t volumePct) {
    gOutput = o;

    digitalWrite(PIN_PA_EN, HIGH);                       // amp off first, always
#if PIN_DAC_XSMT >= 0
    digitalWrite(PIN_DAC_XSMT, LOW);
#endif
    delay(40);

    // MCLK stays on IO4 either way: the ES8311 needs it, the PCM510x ignores it.
    bool routed;
    if (o == Output::Headphones) {
        routed = gAudio.setPinout(PIN_DAC_BCLK, PIN_DAC_LRCK, PIN_DAC_DOUT,
                                  I2S_PIN_NO_CHANGE, PIN_I2S_MCLK);
        releaseI2SPin(PIN_I2S_BCLK);
        releaseI2SPin(PIN_I2S_LRCK);
        releaseI2SPin(PIN_I2S_DOUT);
    } else {
        routed = gAudio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT,
                                  I2S_PIN_NO_CHANGE, PIN_I2S_MCLK);
        releaseI2SPin(PIN_DAC_BCLK);
        releaseI2SPin(PIN_DAC_LRCK);
        releaseI2SPin(PIN_DAC_DOUT);
    }
    if (!routed) Serial.println("[audio] setPinout() FAILED -- i2s_set_pin rejected the pins");

    applyVolume(volumePct);

    if (o == Output::Headphones) {
#if PIN_DAC_XSMT >= 0
        digitalWrite(PIN_DAC_XSMT, HIGH);
#endif
    } else {
        delay(20);
        digitalWrite(PIN_PA_EN, LOW);
    }

    if (gStateLock) {
        xSemaphoreTake(gStateLock, portMAX_DELAY);
        gState.output = o;
        xSemaphoreGive(gStateLock);
    }
    Serial.printf("[player] output -> %s\n", o == Output::Headphones ? "headphones" : "speaker");
    player::diagnoseOutput();
}

void rebuildOrder() {
    gOrder.resize(gQueue.size());
    for (size_t i = 0; i < gOrder.size(); ++i) gOrder[i] = i;
    if (gShuffle && gOrder.size() > 1) {
        for (size_t i = gOrder.size() - 1; i > 0; --i) {
            size_t j = esp_random() % (i + 1);
            std::swap(gOrder[i], gOrder[j]);
        }
    }
}

// Runs on the audio task. Scans a FLAC file's metadata blocks (the header
// region before the audio data starts) for a PICTURE block (type 6) and
// extracts its embedded image bytes. The audio library parses FLAC metadata
// for tag info but never surfaces embedded art at all -- unlike MP3, where
// the library calls audio_id3image() for us -- so this is done independently
// via its own file handle, under the same card lock everything else uses.
std::vector<uint8_t> extractFlacArt(const std::string &path) {
    std::vector<uint8_t> result;

    xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
    File f = SD_MMC.open(path.c_str());
    if (!f) {
        xSemaphoreGiveRecursive(gSdLock);
        Serial.println("[art] FLAC: could not open file");
        return result;
    }

    char magic[4] = {0};
    if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "fLaC", 4) != 0) {
        f.close();
        xSemaphoreGiveRecursive(gSdLock);
        Serial.printf("[art] FLAC: bad magic (got %02X %02X %02X %02X)\n",
                      magic[0], magic[1], magic[2], magic[3]);
        return result;
    }

    auto readU32 = [&f]() -> uint32_t {
        uint8_t b[4] = {0};
        f.read(b, 4);
        return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    };

    bool found = false;
    for (;;) {
        uint8_t header[4];
        if (f.read(header, 4) != 4) break;
        bool    isLast = header[0] & 0x80;
        uint8_t type   = header[0] & 0x7F;
        uint32_t len   = ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];
        Serial.printf("[art] FLAC: block type=%u len=%u last=%d\n", type, (unsigned)len, isLast);

        if (type == 6) {   // PICTURE
            found = true;
            readU32();                              // picture type -- unused
            uint32_t mimeLen = readU32();
            f.seek(f.position() + mimeLen);          // skip MIME string
            uint32_t descLen = readU32();
            f.seek(f.position() + descLen);          // skip description
            readU32(); readU32(); readU32(); readU32(); // width/height/depth/colors -- unused

            uint32_t dataLen = readU32();
            Serial.printf("[art] FLAC: PICTURE block, mimeLen=%u descLen=%u dataLen=%u\n",
                          (unsigned)mimeLen, (unsigned)descLen, (unsigned)dataLen);
            if (dataLen > 0 && dataLen < 4 * 1024 * 1024) {
                result.resize(dataLen);
                size_t got = f.read(result.data(), dataLen);
                Serial.printf("[art] FLAC: read %u of %u requested bytes\n", (unsigned)got, (unsigned)dataLen);
            } else {
                Serial.println("[art] FLAC: dataLen out of bounds, skipping");
            }
            break;
        }

        f.seek(f.position() + len);
        if (isLast) break;
    }
    if (!found) Serial.println("[art] FLAC: no PICTURE block found in this file");

    f.close();
    xSemaphoreGiveRecursive(gSdLock);
    Serial.printf("[art] FLAC: extraction done, %u bytes\n", (unsigned)result.size());
    return result;
}

// Runs on the audio task. Opens a file by path without touching the queue, so
// playback can begin before the folder has been scanned.
void startPath(const std::string &path, const std::string &displayName) {
    publishTitle(displayName, "");
    setPendingArt({});   // clear until we know better, so a track with no art
                         // (or a format we don't extract from) doesn't keep
                         // showing the previous track's art

    uint32_t t0 = millis();
    checkpoint(200);
    xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
    checkpoint(201);
    gAudio.stopSong();
    checkpoint(202);
    bool ok = gAudio.connecttoFS(SD_MMC, path.c_str());
    xSemaphoreGiveRecursive(gSdLock);

    if (ok) {
        // Re-assert the last known volume on every track start. Neither the
        // codec nor the decoder's software volume is otherwise touched except
        // by initCodec()'s boot default and the user's own slider -- nothing
        // here should be able to drift them -- but this makes volume
        // self-healing across track changes regardless of cause, rather than
        // relying on that holding forever. It matters more now that one of
        // the two output paths carries its volume in the decoder, which
        // connecttoFS() has just reinitialised.
        uint8_t vol = 60;
        if (gStateLock) {
            xSemaphoreTake(gStateLock, portMAX_DELAY);
            vol = gState.volume;
            xSemaphoreGive(gStateLock);
        }
        applyVolume(vol);

        publishPath(path);
        publish(true, false);
        Serial.printf("[player] playing %s (open took %lums)\n",
                      path.c_str(), (unsigned long)(millis() - t0));

        // MP3 art arrives asynchronously via audio_id3image() as the library
        // parses ID3 headers over the next few loop() calls. FLAC art isn't
        // exposed by the library at all, so it's pulled directly here.
        std::string lower = toLower(path);
        if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".flac") == 0) {
            setPendingArt(extractFlacArt(path));
        }
    } else {
        Serial.printf("[player] failed to open %s\n", path.c_str());
        publishPath("");
        publish(false, false);
    }
}

// Runs on the audio task.
void startCurrent() {
    if (gOrder.empty()) return;
    const Track &t = gQueue[gOrder[gPos]];
    startPath(t.path, t.name);
}

// --- incremental folder scan ----------------------------------------------
//
// Rebuilding the queue means walking the whole directory, which on a large
// folder takes far longer than the DMA buffers can coast on. Doing it in one
// blocking call starves the decoder and stutters the track that just started.
// So the scan is broken into short slices run between gAudio.loop() calls.

struct Scan {
    bool               active = false;
    File               dir;
    std::string        dirPath;
    std::string        targetPath;
    std::vector<Track> dirs;    // collected too, purely so the cache entry
    std::vector<Track> files;   // this scan produces is complete for listDir()
};
Scan gScan;

// Shared tail end for both the cache-hit (instant) and live-scan paths: takes
// an already-sorted, files-only listing and makes it the playback queue.
void commitQueue(const std::vector<Track> &sortedFiles, const std::string &targetPath) {
    gQueue = sortedFiles;
    if (gQueue.empty()) return;

    size_t target = 0;
    for (size_t i = 0; i < gQueue.size(); ++i) {
        if (gQueue[i].path == targetPath) { target = i; break; }
    }
    rebuildOrder();
    gPos = 0;
    for (size_t i = 0; i < gOrder.size(); ++i) {
        if (gOrder[i] == target) { gPos = i; break; }
    }
}

void scanBegin(const std::string &dirPath, const std::string &targetPath) {
    if (gScan.active) {
        xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
        gScan.dir.close();
        xSemaphoreGiveRecursive(gSdLock);
    }
    gScan.active = false;

    // The UI always browses a folder (populating the cache) before any file
    // in it is tappable, so this hits essentially every time in practice --
    // the card never gets touched at all for the queue rebuild.
    std::vector<Track> cached;
    if (gDirCache.get(dirPath, cached)) {
        std::vector<Track> files;
        files.reserve(cached.size());
        for (const Track &t : cached) if (!t.isDir) files.push_back(t);
        commitQueue(files, targetPath);
        Serial.printf("[player] queue built from cache, %u tracks\n", (unsigned)gQueue.size());
        return;
    }

    gScan.dirs.clear();
    gScan.files.clear();
    gScan.dirPath    = dirPath;
    gScan.targetPath = targetPath;

    xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
    gScan.dir = SD_MMC.open(dirPath.c_str());
    xSemaphoreGiveRecursive(gSdLock);

    gScan.active = gScan.dir && gScan.dir.isDirectory();
}

void scanFinish() {
    xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
    gScan.dir.close();
    xSemaphoreGiveRecursive(gSdLock);
    gScan.active = false;

    auto byName = [](const Track &a, const Track &b) {
        return toLower(a.name) < toLower(b.name);
    };
    std::sort(gScan.dirs.begin(),  gScan.dirs.end(),  byName);
    std::sort(gScan.files.begin(), gScan.files.end(), byName);

    std::vector<Track> combined = gScan.dirs;
    combined.insert(combined.end(), gScan.files.begin(), gScan.files.end());
    gDirCache.put(gScan.dirPath, combined);

    commitQueue(gScan.files, gScan.targetPath);
    gScan.dirs.clear();  gScan.dirs.shrink_to_fit();
    gScan.files.clear(); gScan.files.shrink_to_fit();

    Serial.printf("[player] queue built from card, %u tracks\n", (unsigned)gQueue.size());
}

// One slice. Budget is deliberately well under the DMA buffer depth so the
// decoder is never more than a single loop pass behind. Only runs at all when
// scanBegin() missed the cache, which normal browse-then-tap usage shouldn't.
void scanStep(uint32_t budgetMs = 8) {
    if (!gScan.active) return;

    xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
    uint32_t t0 = millis();
    while (millis() - t0 < budgetMs) {
        File entry = gScan.dir.openNextFile();
        if (!entry) {
            xSemaphoreGiveRecursive(gSdLock);
            scanFinish();
            return;
        }

        std::string name = entry.name();
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        bool isDir = entry.isDirectory();
        entry.close();

        if (name.empty() || name[0] == '.') continue;

        std::string full = gScan.dirPath;
        if (full.empty() || full.back() != '/') full += '/';
        full += name;

        if (isDir) {
            gScan.dirs.push_back({full, name, true});
        } else if (player::isPlayable(name)) {
            gScan.files.push_back({full, stripExtension(name), false});
        }
    }
    xSemaphoreGiveRecursive(gSdLock);
}

void handleCommand(const Cmd &c) {
    switch (c.type) {
    case CmdType::PlayFile: {
        std::string full(c.path);

        size_t slash = full.find_last_of('/');
        std::string dir  = (slash == 0 || slash == std::string::npos)
                           ? "/" : full.substr(0, slash);
        std::string name = full.substr(slash + 1);
        std::string display = stripExtension(name);

        // Audio starts here. Everything else waits.
        startPath(full, display);

        // Queue building is deferred to scanStep(), sliced between decode
        // passes. Until it completes, next/previous simply have nothing to
        // move to, which is a far better failure than a stuttering track.
        gQueue.clear();
        gOrder.clear();
        gPos = 0;
        scanBegin(dir, full);
        break;
    }
    case CmdType::TogglePause: {
        if (gOrder.empty()) return;
        gAudio.pauseResume();
        bool nowPaused = !gAudio.isRunning();
        publish(true, nowPaused);
        break;
    }
    case CmdType::Next:
        if (gOrder.empty()) return;
        gPos = (gPos + 1) % gOrder.size();
        startCurrent();
        break;

    case CmdType::Prev:
        if (gOrder.empty()) return;
        if (gAudio.getAudioCurrentTime() > 3) { startCurrent(); break; }
        gPos = (gPos == 0) ? gOrder.size() - 1 : gPos - 1;
        startCurrent();
        break;

    case CmdType::Stop:
        xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
        gAudio.stopSong();
        xSemaphoreGiveRecursive(gSdLock);
        publish(false, false);
        publishTitle("Nothing playing", "");
        publishPath("");
        break;

    case CmdType::Seek: {
        // Not gAudio.setAudioPlayPosition(): it always derives the target
        // byte from the average bitrate, which the library only ever
        // computes for MP3/WAV/M4A/AAC. For FLAC it's permanently 0, so that
        // call collapses to "seek to m_audioDataStart" regardless of what
        // percent was asked for -- every touch of the seek bar snaps FLAC
        // playback back to 0:00. Seeking by byte fraction across the
        // compressed data instead works for every codec here; the decoder
        // still corrects it to a valid frame boundary internally.
        //
        // Guarded on duration being known: getAudioDataStartPos() can still
        // read as 0 (header not parsed yet) in the brief window right after
        // a track starts, and a "start" of 0 would let a seek target land
        // inside the file's header/metadata instead of the actual audio --
        // not a valid frame boundary at all, for any codec.
        checkpoint(100);
        uint32_t duration = gAudio.getAudioFileDuration();
        uint32_t start    = gAudio.getAudioDataStartPos();
        uint32_t size      = gAudio.getFileSize();
        Serial.printf("[player] seek requested: %u%% duration=%u start=%u size=%u\n",
                      (unsigned)c.a, (unsigned)duration, (unsigned)start, (unsigned)size);
        if (duration && size > start) {
            uint32_t pos = start + (uint32_t)((uint64_t)(size - start) * c.a / 100);
            Serial.printf("[player] seek: target byte pos=%u (codec=%s)\n",
                          (unsigned)pos, gAudio.getCodecname());
            checkpoint(101);
            xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
            checkpoint(102);
            gAudio.setFilePos(pos);
            xSemaphoreGiveRecursive(gSdLock);
        } else {
            Serial.println("[player] seek: skipped, duration/start/size not ready yet");
        }
        break;
    }
    case CmdType::Volume:
        applyVolume((uint8_t)c.a);
        if (gStateLock) {
            xSemaphoreTake(gStateLock, portMAX_DELAY);
            gState.volume = (uint8_t)c.a;
            xSemaphoreGive(gStateLock);
        }
        break;

    case CmdType::SetOutput: {
        uint8_t vol = 60;
        if (gStateLock) {
            xSemaphoreTake(gStateLock, portMAX_DELAY);
            vol = gState.volume;
            xSemaphoreGive(gStateLock);
        }
        applyOutput((Output)c.a, vol);
        break;
    }

    case CmdType::Repeat:
        gRepeat = (RepeatMode)c.a;
        publish(gState.playing, gState.paused);
        break;

    case CmdType::Shuffle: {
        bool on = c.a != 0;
        if (gShuffle != on) {
            gShuffle = on;
            size_t current = gOrder.empty() ? 0 : gOrder[gPos];
            rebuildOrder();
            for (size_t i = 0; i < gOrder.size(); ++i) {
                if (gOrder[i] == current) { gPos = i; break; }
            }
        }
        publish(gState.playing, gState.paused);
        break;
    }
    }
}

void audioTask(void *) {
    // Confirmed by a real watchdog trigger: when a seek hangs this task, the
    // CPU goes idle rather than spinning -- it's blocked on a wait that never
    // gets satisfied, not stuck in a runaway decode loop. The task watchdog
    // below is the backstop (reboots automatically instead of staying stuck),
    // and checkpoint() below leaves a breadcrumb in RTC memory (untouched by
    // the watchdog's software reset) so the exact blocking call is visible on
    // the next boot instead of just knowing it hung somewhere.
    esp_task_wdt_add(NULL);

    Cmd c;
    uint32_t lastProgress = 0;
    bool     lastRunning  = false;   // diagnostic: log isRunning() transitions

    for (;;) {
        // Drain every pending command before decoding again.
        checkpoint(1);
        while (xQueueReceive(gCmdQueue, &c, 0) == pdTRUE) {
            handleCommand(c);
        }

        // Locked against a concurrent cache-miss listDir() on the UI task --
        // rare now that browsing always populates the cache first, but a
        // brand-new never-browsed folder can still race this while something
        // else plays in the background.
        checkpoint(2);
        xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
        checkpoint(3);
        gAudio.loop();
        checkpoint(4);
        xSemaphoreGiveRecursive(gSdLock);

        // A slice of any pending folder scan, sized to fit comfortably inside
        // the DMA buffer depth. No-op (and no card access) on the common path
        // where scanBegin() already resolved the queue from the cache.
        checkpoint(5);
        scanStep();

        checkpoint(6);
        if (millis() - lastProgress >= 200) {
            lastProgress = millis();
            publishProgress();
        }

        // Diagnostic: catches exactly when/if the decoder stops running,
        // e.g. right after a seek, without waiting on a 200ms poll window.
        bool running = gAudio.isRunning();
        if (running != lastRunning) {
            lastRunning = running;
            Serial.printf("[player] isRunning -> %s at t=%ums (elapsed=%u/%u)\n",
                          running ? "true" : "false", (unsigned)millis(),
                          (unsigned)gAudio.getAudioCurrentTime(),
                          (unsigned)gAudio.getAudioFileDuration());
        }

        esp_task_wdt_reset();   // pet the watchdog: this iteration completed
        vTaskDelay(1);
    }
}

void post(CmdType type, uint32_t a = 0, const char *path = nullptr) {
    if (!gCmdQueue) return;
    Cmd c;
    c.type = type;
    c.a    = a;
    if (path) strlcpy(c.path, path, sizeof(c.path));
    xQueueSend(gCmdQueue, &c, 0);
}

} // namespace

// Callbacks fire inside gAudio.loop(), so they run on the audio task and may
// touch the playlist directly.
void audio_id3data(const char *info) {
    std::string s(info);
    if (s.rfind("Title:", 0) == 0 && s.size() > 6) {
        xSemaphoreTake(gStateLock, portMAX_DELAY);
        gState.title = s.substr(6);
        xSemaphoreGive(gStateLock);
    }
    if (s.rfind("Artist:", 0) == 0 && s.size() > 7) {
        xSemaphoreTake(gStateLock, portMAX_DELAY);
        gState.artist = s.substr(7);
        xSemaphoreGive(gStateLock);
    }
}

// Called by the library when it finds an ID3 APIC frame while parsing an
// MP3's header, on the audio task, with the file positioned wherever it last
// happened to be -- pos/size cover the frame's raw content, not just the
// image (see the parse below). Runs synchronously inside gAudio.loop(), so
// this needs to be quick; a single track's cover art is at most a few
// hundred KB and this only runs once per track.
void audio_id3image(File &file, const size_t pos, const size_t size) {
    Serial.printf("[art] MP3: audio_id3image called, pos=%u size=%u\n", (unsigned)pos, (unsigned)size);
    if (size < 4 || size > 2 * 1024 * 1024) {
        Serial.println("[art] MP3: size out of bounds, skipping");
        setPendingArt({});
        return;
    }

    // APIC frame content: 1 byte text encoding, then a MIME type string,
    // then 1 byte picture type, then a description string, then the raw
    // image data for the remainder. Encodings 1/2 are UTF-16 (2-byte units,
    // terminated by a double NUL); 0/3 are single-byte, NUL-terminated.
    xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
    file.seek(pos);
    int encoding = file.read();
    bool wide = (encoding == 1 || encoding == 2);

    int c;
    while ((c = file.read()) > 0) {}          // MIME string (always single-byte)
    file.read();                              // picture type byte

    if (wide) {
        int b1 = 1, b2 = 1;
        while (!(b1 <= 0 && b2 <= 0)) {
            b1 = file.read();
            b2 = file.read();
        }
    } else {
        int d;
        while ((d = file.read()) > 0) {}
    }

    size_t consumed = file.position() - pos;
    xSemaphoreGiveRecursive(gSdLock);
    Serial.printf("[art] MP3: encoding=%d consumed=%u header bytes\n", encoding, (unsigned)consumed);

    if (consumed >= size) {
        Serial.println("[art] MP3: header consumed >= frame size, nothing left for image data");
        setPendingArt({});
        return;
    }
    size_t imgSize = size - consumed;

    std::vector<uint8_t> bytes(imgSize);
    xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);
    file.seek(pos + consumed);
    size_t got = file.read(bytes.data(), imgSize);
    xSemaphoreGiveRecursive(gSdLock);

    Serial.printf("[art] MP3: read %u of %u image bytes, first 4 = %02X %02X %02X %02X\n",
                  (unsigned)got, (unsigned)imgSize,
                  imgSize > 0 ? bytes[0] : 0, imgSize > 1 ? bytes[1] : 0,
                  imgSize > 2 ? bytes[2] : 0, imgSize > 3 ? bytes[3] : 0);

    setPendingArt(std::move(bytes));
}

void audio_eof_mp3(const char *) {
    if (gRepeat == RepeatMode::One) { startCurrent(); return; }
    if (gPos + 1 < gOrder.size())   { gPos++; startCurrent(); return; }
    if (gRepeat == RepeatMode::All && !gOrder.empty()) {
        gPos = 0;
        startCurrent();
        return;
    }
    publish(false, false);
}

namespace player {

bool initCodec() {
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, HIGH);          // keep the amp muted during init

    // Park the external DAC's lines low so it boots muted -- with no clock it
    // mutes itself, whether or not XSMT is wired.
    releaseI2SPin(PIN_DAC_BCLK);
    releaseI2SPin(PIN_DAC_LRCK);
    releaseI2SPin(PIN_DAC_DOUT);
#if PIN_DAC_XSMT >= 0
    pinMode(PIN_DAC_XSMT, OUTPUT);
    digitalWrite(PIN_DAC_XSMT, LOW);
#endif

    gCodecOk = gCodec.begin(Wire, ES8311_I2C_ADDR);
    if (!gCodecOk) {
        Serial.println("[player] codec init FAILED");
        return false;
    }

    gCodec.setVolume(60);
    delay(200);                             // settle before unmuting the amp
    digitalWrite(PIN_PA_EN, LOW);
    return true;
}

bool begin() {
    // If the audio task's checkpoint survived from before this boot (RTC
    // memory isn't touched by a watchdog's software reset), it means the
    // last run ended in a hang -- report exactly which step it was on, then
    // clear it so a later unrelated reset doesn't show stale data.
    // ...but only believe it if the CPU was actually stopped against its will.
    // RTC memory survives far more than a watchdog: an RTS-driven reset from
    // the flashing tool, the reset button, a brownout blip -- all leave the
    // last checkpoint sitting there looking like a crash site. The audio task
    // spends essentially all of its time on one or two checkpoints, so
    // reporting unconditionally meant every ordinary reflash "hung at
    // checkpoint 6", which is just where the loop happened to be. That was
    // noise, and worse, it would have buried a real hang among the false ones.
    esp_reset_reason_t why = esp_reset_reason();
    bool wasKilled = why == ESP_RST_PANIC   || why == ESP_RST_TASK_WDT ||
                     why == ESP_RST_INT_WDT || why == ESP_RST_WDT;
    if (gCheckpointMagic == kCheckpointMagic && wasKilled) {
        Serial.printf("[player] previous run hung at checkpoint %u: %s (reset reason=%d)\n",
                      (unsigned)gCheckpoint, checkpointName(gCheckpoint), (int)why);
    }
    gCheckpointMagic = 0;
    gCheckpoint = 0;

    if (!gStateLock) gStateLock = xSemaphoreCreateMutex();
    if (!gCmdQueue)  gCmdQueue  = xQueueCreate(8, sizeof(Cmd));
    if (!gSdLock)    gSdLock    = xSemaphoreCreateRecursiveMutex();
    if (!gArtLock)   gArtLock   = xSemaphoreCreateMutex();
    gDirCache.init();

    gState.title = "Nothing playing";

    initCodec();

    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0,
                   PIN_SD_D1,  PIN_SD_D2,  PIN_SD_D3);

    // Try 4-bit at 40MHz first. Each fallback costs real throughput, so the
    // mode that actually took is recorded and reported at boot.
    gCardOk = SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_HIGHSPEED);
    if (gCardOk) {
        gSdMode = "4-bit @ 40MHz";
    } else {
        gCardOk = SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT);
        if (gCardOk) {
            gSdMode = "4-bit @ 20MHz";
        } else {
            gCardOk = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);
            if (gCardOk) gSdMode = "1-bit @ 20MHz (slow)";
        }
    }
    Serial.printf("[player] SD: %s\n", gSdMode);
    if (gCardOk) {
        Serial.printf("[player] card mounted, %llu MB\n",
                      SD_MMC.cardSize() / (1024ULL * 1024ULL));
    } else {
        Serial.println("[player] SD mount FAILED (card inserted? formatted FAT32?)");
    }

    // Tag 3.0.0 signature: (BCLK, LRC, DOUT, DIN, MCLK). DIN is the microphone
    // path, unused here.
    gAudio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT,
                     I2S_PIN_NO_CHANGE, PIN_I2S_MCLK);
    gAudio.setVolume(21);                   // library range is 0-21; the ES8311
                                            // does the actual attenuation

    // Generous timeout: normal iterations complete in well under a second,
    // so this only ever fires on a genuine stall, not a legitimately slow
    // pass (e.g. a big fallback SD scan). Ignore the return code -- if a
    // watchdog is already active (Arduino's own default), this just leaves
    // it as-is and esp_task_wdt_add() below still subscribes us to it.
    esp_task_wdt_init(10, true);

    // Decoding lives on core 0 so LVGL keeps core 1 to itself.
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 5, nullptr, 0);

    analogReadResolution(12);
    return gCodecOk && gCardOk;
}

bool codecOk() { return gCodecOk; }
bool cardOk()  { return gCardOk;  }
const char *sdModeString() { return gSdMode; }

bool isPlayable(const std::string &filename) {
    std::string n = toLower(filename);
    static const char *exts[] = {".mp3", ".flac", ".wav", ".m4a", ".aac", ".ogg", ".opus"};
    for (const char *e : exts) {
        size_t len = strlen(e);
        if (n.size() > len && n.compare(n.size() - len, len, e) == 0) return true;
    }
    return false;
}

std::vector<Track> listDir(const std::string &path) {
    std::vector<Track> cached;
    if (gDirCache.get(path, cached)) return cached;

    std::vector<Track> dirs, files;
    if (!gCardOk) return dirs;

    xSemaphoreTakeRecursive(gSdLock, portMAX_DELAY);

    File root = SD_MMC.open(path.c_str());
    if (!root || !root.isDirectory()) {
        xSemaphoreGiveRecursive(gSdLock);
        return dirs;
    }

    File entry;
    while ((entry = root.openNextFile())) {
        std::string name = entry.name();
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);

        if (name.empty() || name[0] == '.') { entry.close(); continue; }

        std::string full = path;
        if (full.empty() || full.back() != '/') full += '/';
        full += name;

        if (entry.isDirectory()) {
            dirs.push_back({full, name, true});
        } else if (isPlayable(name)) {
            files.push_back({full, stripExtension(name), false});
        }
        entry.close();
    }
    root.close();

    xSemaphoreGiveRecursive(gSdLock);

    auto byName = [](const Track &a, const Track &b) { return toLower(a.name) < toLower(b.name); };
    std::sort(dirs.begin(),  dirs.end(),  byName);
    std::sort(files.begin(), files.end(), byName);
    dirs.insert(dirs.end(), files.begin(), files.end());

    gDirCache.put(path, dirs);
    return dirs;
}

void playFile(const std::string &fullPath) {
    post(CmdType::PlayFile, 0, fullPath.c_str());
}

void togglePause()               { post(CmdType::TogglePause); }
void next()                      { post(CmdType::Next); }
void previous()                  { post(CmdType::Prev); }
void stop()                      { post(CmdType::Stop); }
void seekPercent(uint8_t pct)    { post(CmdType::Seek, pct); }
void setVolume(uint8_t pct)      { post(CmdType::Volume, pct > 100 ? 100 : pct); }
void setOutput(Output o)         { post(CmdType::SetOutput, (uint32_t)o); }
void setRepeat(RepeatMode m)     { post(CmdType::Repeat, (uint32_t)m); }
void setShuffle(bool on)         { post(CmdType::Shuffle, on ? 1 : 0); }

PlayerState snapshot() {
    PlayerState s;
    if (!gStateLock) return s;
    xSemaphoreTake(gStateLock, portMAX_DELAY);
    s = gState;
    xSemaphoreGive(gStateLock);
    return s;
}

bool pickupArt(uint32_t &lastSeenGeneration, std::vector<uint8_t> &out) {
    if (!gArtLock) return false;
    xSemaphoreTake(gArtLock, portMAX_DELAY);
    bool changed = gPendingArt.generation != lastSeenGeneration;
    if (changed) {
        lastSeenGeneration = gPendingArt.generation;
        out = std::move(gPendingArt.bytes);
        gPendingArt.bytes.clear();
        gPendingArt.bytes.shrink_to_fit();
    }
    xSemaphoreGive(gArtLock);
    if (changed) {
        Serial.printf("[art] pickupArt: UI picked up generation %u, %u bytes\n",
                      (unsigned)lastSeenGeneration, (unsigned)out.size());
    }
    return changed;
}

void diagnoseOutput() {
    uint8_t vol = 0;
    if (gStateLock) {
        xSemaphoreTake(gStateLock, portMAX_DELAY);
        vol = gState.volume;
        xSemaphoreGive(gStateLock);
    }
    bool hp = gOutput == Output::Headphones;

    Serial.println("[audio] ---- output diagnostics ----");
    Serial.printf("[audio] selected      : %s\n", hp ? "HEADPHONES (PCM510x)" : "SPEAKER (ES8311)");
    Serial.printf("[audio] decoder running: %s, volume %u%%\n",
                  gAudio.isRunning() ? "yes" : "no", (unsigned)vol);
    Serial.printf("[audio] attenuation   : %s\n",
                  hp ? "decoder software volume (PCM510x has no volume register)"
                     : "ES8311 register, decoder at full scale");
    Serial.printf("[audio] amp PA_EN IO%d: %s\n", PIN_PA_EN,
                  digitalRead(PIN_PA_EN) ? "HIGH (amp muted)" : "LOW (amp enabled)");
#if PIN_DAC_XSMT >= 0
    Serial.printf("[audio] XSMT IO%d      : %s\n", PIN_DAC_XSMT,
                  digitalRead(PIN_DAC_XSMT) ? "HIGH (unmuted)" : "LOW (muted)");
#else
    Serial.println("[audio] XSMT          : not wired -- the module must hold it high itself");
#endif

    // Exactly one of these two groups should show the I2S signals; the other
    // should read as detached. Both driven means the release didn't take and
    // both DACs are being clocked; neither means setPinout() didn't take.
    Serial.println("[audio] codec pins (ES8311):");
    reportPin("BCLK", PIN_I2S_BCLK, I2S0O_BCK_OUT_IDX);
    reportPin("LRCK", PIN_I2S_LRCK, I2S0O_WS_OUT_IDX);
    reportPin("DOUT", PIN_I2S_DOUT, I2S0O_SD_OUT_IDX);
    Serial.println("[audio] external DAC pins (PCM510x):");
    reportPin("BCLK", PIN_DAC_BCLK, I2S0O_BCK_OUT_IDX);
    reportPin("LRCK", PIN_DAC_LRCK, I2S0O_WS_OUT_IDX);
    reportPin("DOUT", PIN_DAC_DOUT, I2S0O_SD_OUT_IDX);
    Serial.println("[audio] -----------------------------");
}

bool lockCard(uint32_t timeoutMs) {
    if (!gSdLock) return false;
    return xSemaphoreTakeRecursive(gSdLock, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void unlockCard() {
    if (gSdLock) xSemaphoreGiveRecursive(gSdLock);
}

float batteryVolts() {
    // IO9 sits behind a 2:1 divider, so double the measured voltage.
    uint32_t mv = analogReadMilliVolts(PIN_BAT_ADC);
    return (mv * 2.0f) / 1000.0f;
}

} // namespace player
