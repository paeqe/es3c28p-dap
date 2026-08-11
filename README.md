# ES3C28P DAP

A touch-driven music player for the 2.8" ESP32-S3 display board (ILI9341V panel,
FT6336G touch, ES8311 codec, FM8002E amp, SDIO microSD).

Plays MP3, FLAC, WAV, M4A/AAC, OGG and Opus from the card, with a folder
browser, a now-playing screen, shuffle, repeat, seek and volume. Two of them
in the same room can squirt tracks to each other.

## Layout

```
platformio.ini
include/
  board_config.h     pin map from the lcdwiki IO table
  lv_conf.h          you create this (see below)
src/
  main.cpp           display init, LVGL bridge, touch, startup
  display.h          LovyanGFX device description
  es8311.h/.cpp      codec driver (playback path only)
  player.h/.cpp      SD, playlist, transport, audio task
  squirt.h/.cpp      device-to-device track transfer (ESP-NOW, encrypted)
  ui.h/.cpp          LVGL browser, now-playing and squirt screens
```

## LVGL's config header

LVGL deliberately doesn't ship `lv_conf.h`, so `include/lv_conf.h` is committed
here with the settings below already applied — `pio run` should work straight
from a clone.

If you ever need to regenerate it from scratch:

```bash
pio run                                   # fails, but fetches the libraries
cp .pio/libdeps/es3c28p/lvgl/lv_conf_template.h include/lv_conf.h
```

Then edit `include/lv_conf.h`:

- change the `#if 0` on line 15 to `#if 1`
- `LV_COLOR_DEPTH` → `16`
- `LV_COLOR_16_SWAP` → `0` (LovyanGFX byte-swaps for the bus itself; setting
  this to `1` swaps twice and scrambles every colour)
- `LV_MEM_CUSTOM` → `1` (uses the ESP heap, so PSRAM is available)
- `LV_FONT_MONTSERRAT_14` → `1`
- `LV_USE_PERF_MONITOR` → `1` while you are tuning, `0` afterwards

## Wiring

Everything is on-board except the speaker. It goes on the 1.25mm 2P header
marked `SPEAKER` — 1.5W at 8Ω or 2W at 4Ω. A 3.7V LiPo goes on the `BAT`
header; watch the polarity, it is silkscreened but easy to get backwards.

For a headphone jack, see the GY-PCM510x section below.

## Headphones (GY-PCM5100/5101/5102)

The onboard ES8311 drives the speaker header. For a headphone jack, add one of
the GY-PCM510x I2S DAC modules — they're the same board with a different DAC
fitted, and all three work here.

It gets its own three pins rather than sharing the codec's bus:

```
GY-PCM510x        ES3C28P
  VIN      <---   5V   <- not 3V3, see below
  GND      <---   GND
  BCK      <---   IO2   (PIN_DAC_BCLK)
  LCK      <---   IO3   (PIN_DAC_LRCK)
  DIN      <---   IO14  (PIN_DAC_DOUT)
  SCK      <---   GND                    <- important, see below
  XSMT     <---   3V3                    <- also required, see below
```

That 5V and that XSMT tie are both load-bearing — this was silent with VIN on
3V3 and XSMT left to the module, and came straight up once both were fixed.

**Feed VIN from 5V, not 3V3.** These modules put an LDO in front of the DAC,
and the DAC wants 3.3V ±10% — so it depends entirely on which regulator your
board happens to have fitted. An XC6206 (marked `662K`) drops ~0.25V and would
just scrape by from 3V3; an AMS1117-3.3 drops over a volt and leaves the DAC at
roughly 2.2V, which is well under spec and simply won't play. 5V in is correct
for both. If a module is silent with everything else right, measure the DAC's
supply pin before assuming anything else — it should read close to 3.3V.

**Tie SCK to GND.** That tells the PCM510x to run its internal PLL off BCK
instead of expecting a master clock. The ES8311 still needs the real MCLK on
IO4 and still gets it — the PCM510x simply ignores it. Leaving SCK floating
gives you silence or noise and is the single most common way to get one of
these modules wrong.

**Tie XSMT to 3V3.** It is the DAC's soft mute and low means muted, always —
so if the module doesn't hold it high itself, nothing ever plays no matter how
correct everything else is. Not all of these boards fit that pull-up, and this
one doesn't; a bare wire to 3V3 is enough.

It only needs tying, not driving, so `PIN_DAC_XSMT` stays `-1`. There is one
I2S peripheral and two DACs on separate pins, so switching output re-routes
that peripheral to one set of pins and cuts the other loose — and a PCM510x
with no clock detects the loss and mutes itself. The clock is the mute. Point
`PIN_DAC_XSMT` at a GPIO only if you want a second, explicit one.

The subtle part, if you ever touch `applyOutput()`: `i2s_set_pin()` (which is
all `Audio::setPinout()` does) only ever *adds* a route through the GPIO
matrix — it never removes the previous one. So each switch also has to
explicitly detach the pins it just left, or both DACs stay clocked and both
play at once. `releaseI2SPin()` is what does that.

**IO3 is a strapping pin** on the ESP32-S3 (JTAG source select). It is sampled
at reset and then free to use as an output, and the module's LCK input is high
impedance so it won't disturb that — but if you ever see odd boot behaviour
after wiring this up, IO3 is the first thing to suspect. IO2 and IO14 have no
such caveat.

**The module's other header is configuration and normally needs nothing.**
These boards set FLT / DEMP / XSMT / FMT with their own onboard pull
resistors or solder jumpers, which is why the six-pin header alone is the
usual recipe. Two of those four have to be right for any sound at all, so if
the jack stays silent with SCK correctly grounded, meter them before
suspecting the firmware:

- **XSMT must read high.** It is the soft mute; low means muted, always.
- **FMT must read low.** That selects I2S; high is left-justified, which the
  ESP32 is not sending, and gives you noise or a badly shifted signal.

`DEMP` (de-emphasis) and `FLT` (filter roll-off) only affect tone slightly and
are fine either way.

Switch outputs with the **SPK/HP** button on the now-playing screen. The
selection isn't saved yet, so it starts on the speaker every boot (NVS is on
the list below).

**Volume works differently on each path**, which is worth knowing before you
go looking for a bug. The ES8311 has a real volume register, so on the speaker
the decoder runs at full scale and the codec attenuates. The PCM510x parts on
these modules are the A-suffix versions with no I2C at all — there is no
volume register to write — so on headphones the decoder scales the samples
itself instead (the audio library's 0-21 software volume). One slider, two
mechanisms, and never both at once, since attenuating twice would just throw
away bit depth. Expect slightly coarser volume steps on headphones as a
result.

The jack is line-level (~2 Vrms), which is loud into low-impedance headphones
and fine into powered speakers or an amp. There is no headphone amplifier on
these modules, so high-impedance cans will be quieter than you'd like.

## Card layout

Anything works — the browser walks whatever folder tree is there. Tidiest is:

```
/Music/Artist/Album/01 Track.mp3
```

Format the card as FAT32. exFAT will not mount.

**Album art.** Decoded on-device from MP3 ID3 `APIC` frames and FLAC
`PICTURE` metadata blocks (JPEG or PNG), shown in the now-playing screen's art
panel; falls back to the placeholder icon for tracks with none, and for other
formats (WAV/M4A/AAC/OGG/Opus — not currently parsed for embedded art at all).
Decoding is at full resolution then scaled down for display, so very large
embedded art (well past 1000px) costs real PSRAM and a moment of decode time
per track change. If your files carry unusually large cover art, run
`tools/compress_art.py` over a folder before copying it to the card:

```bash
pip install mutagen pillow
python tools/compress_art.py D:\Music\ToCopy --dry-run   # preview first
python tools/compress_art.py D:\Music\ToCopy             # then apply
```

It resizes embedded art down to ~160px (matching the size the now-playing
screen's art panel actually needs) and drops any extra embedded pictures
(back cover, liner notes) down to just the front cover, which is usually a
large cut in card space per track and makes art decode close to instant. It
overwrites files in place, so point it at a copy if you want to keep
full-resolution art anywhere else.

Add `--watch` to leave it running and have it catch files automatically as
you copy them in, instead of remembering to re-run it:

```bash
python tools/compress_art.py D:\Music\ToCopy --watch
```

It compresses whatever's already there, then keeps polling and picks up new
or changed files as they land -- including mid-copy, since it waits for a
file's size to stop changing before touching it. Ctrl+C to stop.

## Squirt

Hand the track you're playing to another ES3C28P in the room. Every device
runs the same firmware and is symmetric — it beacons, it listens, and it can
be either end of a transfer — so there is nothing to configure and no network
to join. Roles are decided per transfer by whoever taps send.

Open it with the upload icon on the now-playing screen. The radio comes up
with the screen and goes back down when you leave, so a device that isn't
actively squirting isn't on the air at all. Nearby devices appear in the list
within a couple of seconds; tap one to offer it the playing track.

Then both users have to agree:

1. The same six digits appear on both screens. Compare them **out loud** and
   tap Match on both — reading them off the radio would defeat the point.
2. The receiving user gets the filename and size, and accepts or declines.

Files land in `/Squirt` and are **never played automatically** — that one is
deliberate, see below. Nothing is remembered between transfers, so you confirm
every time; the upside is there's no stored key material to steal.

Transfers run over ESP-NOW at roughly 100–150 KB/s, so a typical MP3 takes
under a minute and a big FLAC takes several. It shares the card with the
decoder, so expect playback to be less happy during a transfer — pausing
first is not a bad idea.

The close-out is retried, and it has to be. The data chunks have a sliding
window and get resent until acknowledged, but the two frames that end a
transfer — `FIN` ("that was all of it, here's the hash") and `RESULT` ("saved
it") — are single frames on the same lossy radio. Sent once, either can be
dropped: lose `FIN` and the receiver sits on a complete file it was never told
to keep, until it times out and deletes it; lose `RESULT` and the sender
reports a failure for a transfer that actually worked. So `FIN` now repeats
until answered, and the receiver keeps its session alive for a few seconds
after saving purely to re-answer a repeated `FIN` with the same reply. If you
change that path, keep it idempotent — a second `FIN` must never redo the
write or the rename.

### What the security actually does

Everything arriving over the radio is untrusted input from an unauthenticated
stranger until proven otherwise, and the design follows from that. The full
reasoning is at the top of `src/squirt.cpp`; the short version:

- **Nothing moves without both users.** There is no silent-accept path, so
  nobody can push a file onto a device they aren't standing next to.
- **The six digits are a short authentication string**, the same idea as
  Bluetooth numeric comparison. Both ends do an ECDH exchange and derive the
  code from the *shared secret*. Anyone relaying between two devices has to
  substitute their own key on each side, ends up with two different secrets,
  and cannot make both screens show the same number. A mismatch is what an
  attacker looks like — which is exactly why the code has to be compared out
  loud rather than sent over the air.
- **Everything after that is AES-256-GCM**, keyed from that secret, with a
  separate key per direction, a fresh nonce per frame from a counter that only
  goes up, the header authenticated as additional data, and stale sequence
  numbers dropped. Frames can't be read, replayed, reordered or edited.
  A frame's tag is checked before its contents are looked at at all.
- **Received files are treated as hostile.** The sender's filename is reduced
  to a basename, filtered to a conservative character set, rejected if it's
  hidden or lacks a playable extension, and never allowed to overwrite
  anything. Content goes to a temp file, is checked against a SHA-256 the
  sender commits to, and only gets its real name if it matches.
- **Received files are never played automatically.** Handing attacker-chosen
  bytes straight to an audio decoder is how a file transfer becomes code
  execution, and the decoders here have a track record — see the FLAC
  bit-reader bugs below. A human choosing to play a file is a much better gate
  than a device doing it on arrival. Worth keeping if you touch this code.
- **Bounded everything**: one transfer at a time, capped file size, capped
  peer table, every state times out, malformed frames dropped without reply.
  Someone in range can waste airtime; they can't wedge the device.

What it does *not* do: hide that a transfer is happening, or hide device names
in beacons — those are plaintext by necessity, since they're how devices find
each other before any key exists. And it trusts you to actually compare the
digits rather than tapping through.

One implementation note if you ever port this: `mbedtls_hkdf()` does not link
against the mbedtls the Arduino core ships (`MBEDTLS_HKDF_C` is off, so the
header is there but the symbol isn't). `squirt.cpp` implements HKDF on
`mbedtls_md_hmac()` instead, which is always available.

## Things that will probably bite you

**The ES8311 init sequence is the risky part.** The register writes here follow
the datasheet's standard playback path with MCLK at 256×fs, which is what the
audio library requests. If you get silence but the serial log shows the chip ID
reading back, the codec is alive and it is the clock configuration that is off —
compare against Espressif's `esp_codec_dev` component, which has a full
coefficient table for other MCLK ratios.

**Amplifier pop on boot.** `PIN_PA_EN` is held high through init and dropped low
once the codec is configured. If you still get a pop, add a few hundred ms
between codec init and enabling the amp.

**Seeking used to hang the player.** The pinned ESP32-audioI2S 3.0.0 has two
compounding bugs: `flac_correctResumeFilePos()`/`mp3_correctResumeFilePos()`
search forward from a seek target for the next frame syncword using
`while(!found || pos == m_file_size)` instead of `&&`, so the search never
terminates if it runs off the end of the file. And that search only checks 2
bytes to call something a syncword — not nearly enough to rule out a
coincidental match in real compressed audio — so even when it "succeeds" it
can hand the FLAC decoder a false match. `flac_decoder.cpp`'s Rice-code
reader then decodes garbage from that position by reading one bit at a time
until a terminating 1 bit, with no bound on how many 0 bits it'll accept
first, and its underlying byte reader keeps reading past the end of the
buffer forever once real data runs out rather than stopping. Any of these
could hang the audio task permanently: playback, transport buttons, and
starting new tracks all stop responding; only UI that doesn't route through
the audio task (e.g. Back) keeps working. On top of that, a *successful*
FLAC seek still didn't move the displayed position: `m_audioCurrentTime`
(elapsed time / the progress bar) is only updated after a seek `if
(m_avr_bitrate)`, a field the library never sets for FLAC, so the bar looked
like it snapped back / never moved even when playback genuinely jumped.

The one that actually mattered most in practice, though, took a task
watchdog and an RTC-memory checkpoint (both left in `player.cpp`) to find:
`mutex_audio` (`Audio.cpp`) is created with `xSemaphoreCreateMutex()` — a
plain, non-recursive mutex — but `connecttoFS()` (called on every track
start) takes it with `xSemaphoreTakeRecursive()` and releases it with the
plain `xSemaphoreGive()` at every exit point. Mixing the two APIs on one
handle is invalid: the plain give really does release the underlying mutex,
but only the recursive give decrements FreeRTOS's internal recursion
counter, so that counter is left stuck above zero after every single track
start. The corruption stays latent until something else takes the same
mutex recursively — which `setFilePos()`, the seek codepath, does — at which
point its matching recursive give can't get the counter back to zero either,
so it never actually releases the mutex. The next plain
`xSemaphoreTake(mutex_audio, portMAX_DELAY)` — the first line of
`Audio::loop()`, run every single iteration — then blocks forever. Confirmed
against real hardware: a task-watchdog trigger showed CPU 0 sitting in
`IDLE0` (a genuine blocked wait, not a runaway loop) right as this would
predict, and the RTC checkpoint showed the audio task stuck on exactly that
line. `connecttospeech()` has the identical bug; fixed alongside it even
though this project doesn't use TTS.

`scripts/patch_libs.py` fixes all of this automatically on every build
(wired up via `extra_scripts` in `platformio.ini`), since `.pio/libdeps` is
gitignored and a hand-edit there wouldn't survive a clean rebuild. If you
ever bump the library version, check whether these bugs (and the patches)
still apply.

**`lv_img_set_zoom()` silently does nothing for embedded JPEG art.** LVGL only
applies `draw_dsc->zoom` on the branch of `lv_draw_img.c`'s `decode_and_draw()`
taken when the decoder hands back a fully decoded buffer (`img_data != NULL`).
When `img_data` is NULL it falls through to a read-line-by-line branch that
blits each source row 1:1 and never looks at zoom at all. `lv_sjpg` *always*
sets `img_data = NULL` for in-memory JPEG (`decoder_open`, the
`LV_IMG_SRC_VARIABLE` + `is_jpg` case) — which is every piece of embedded art
here — so no amount of zooming or resizing the widget will scale it. `ui.cpp`
therefore decodes and resamples art to panel size itself
(`scaleArtToPanel()`) and hands LVGL an already-correct-size `TRUE_COLOR`
image. Two related traps in the same area: `lv_obj_set_size()`/`set_pos()`
only mark the layout dirty rather than updating `obj->coords`, so anything
reading geometry back needs an `lv_obj_update_layout()` first; and the image
cache is keyed on the source *pointer*, so reusing one `lv_img_dsc_t` across
tracks serves a stale image unless you call `lv_img_cache_invalidate_src()`.

**Some album art wouldn't decode, and it wasn't progressive JPEG.** TJpgDec
(what `lv_sjpg` wraps) rejects a Y sampling factor of `0x12` outright —
4:4:0, chroma subsampled vertically instead of horizontally. It's unusual but
entirely ordinary baseline JPEG, and a fair bit of embedded art is encoded
that way, which showed up as `jd_prepare failed: rc=8` (`JDR_FMT3`) and a
placeholder icon.

Nothing about 1x2 is genuinely beyond the decoder. Block loading, buffer
sizing and the IDCT are all written in terms of `msx`/`msy` and handle it
unchanged; only two lines in `mcu_output()` are hardcoded for the 2x2 case —
the chroma base offset assumes four Y blocks, and the second luma block row
assumes the next row starts two blocks in. `scripts/patch_libs.py` fixes both
in terms of `msx`/`msy` and relaxes the check. Both expressions reduce to the
original constants when `msx == msy == 2`, so 4:4:4, 4:2:0 and 4:2:2 decode
byte-for-byte as before.

`tools/compress_art.py` also sidesteps this by re-encoding through Pillow,
which emits 4:2:0 — useful if you'd rather shrink oversized art anyway.

**Task watchdog on the audio task.** Given how many of the above were only
findable by actually triggering them on hardware, `player.cpp` also
subscribes the audio task to the ESP-IDF task watchdog (10s timeout, panics
on trigger) as a backstop — if it ever stops making progress for any reason,
including one still undiscovered, the device reboots itself automatically
instead of sitting silently stuck until someone notices. It also leaves a
checkpoint in RTC memory (untouched by a watchdog's software reset) noting
which step it was on, printed at the next boot, so a future hang is
diagnosable from the serial log alone.

That report is gated on `esp_reset_reason()` naming an actual panic or
watchdog. RTC memory survives plenty of resets that aren't crashes — the
RTS-driven reset every `pio run -t upload` ends with, the reset button, a
brownout — and the audio task spends nearly all its time sitting on one or
two checkpoints, so an ungated report announced a hang at whichever one the
loop happened to be on after every single reflash. Convincing, and entirely
false. The danger of that isn't the noise, it's that a real hang would have
been indistinguishable from it.

**Diagnostics.** Hold BOOT across a reset for the touch test (tap BOOT to leave
it) followed by the tone test. A plain reset boots straight into the UI. The
tone test drives I2S port 1, because the audio library's `Audio` object is a
global and claims port 0 before `setup()` runs.

**Library version pinning matters more than usual here.** ESP32-audioI2S tag
3.0.7 and later switched to the IDF 5 `i2s_std` driver, which needs Arduino core
3.x. The official `espressif32` PlatformIO platform tops out at core 2.0.17, so
those tags will not compile against it. Tag `3.0.0` is the last release on the
legacy I2S driver and is what this project pins. Its `setPinout()` takes five
arguments — `(BCLK, LRC, DOUT, DIN, MCLK)` — where later versions dropped `DIN`.
If you ever move to core 3.x via the pioarduino platform fork, bump to `3.4.7`
and drop the `I2S_PIN_NO_CHANGE` argument.

**SD speed.** If playback stutters on FLAC, the card is the usual cause. Try
`SDMMC_FREQ_HIGHSPEED` in `player.cpp`, and use a card rated A1 or better.

**PSRAM.** The LVGL draw buffers come from PSRAM, so `qio_opi` memory type in
`platformio.ini` is not optional on this board. If it is wrong, `heap_caps_malloc`
returns null and the code silently drops to a much smaller internal buffer with
visibly slower redraws.

## Obvious next steps

- Album art for M4A/AAC (MP4 `covr` atom) and OGG/Opus (embedded picture
  comment) — currently MP3 and FLAC only.
- Persist volume, shuffle, output choice and last-played position to NVS.
- Use the WS2812B on IO42 as a playback indicator.
- Deep sleep on idle, wake on touch — the ES8311 has its own low-power mode
  worth enabling at the same time.
