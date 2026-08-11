"""
Fixes real bugs in pinned third-party libraries (ESP32-audioI2S and LVGL).

.pio/libdeps is gitignored and rebuilt from the pinned versions, so editing
the fetched copy directly does not survive a clean rebuild. This runs on
every build and re-applies these fixes whenever a library is (re)fetched.

===============================================================================
ESP32-audioI2S 3.0.0
===============================================================================

--- Bug 1: the frame-syncword search after a seek never terminates ---------

Audio::flac_correctResumeFilePos() and Audio::mp3_correctResumeFilePos() scan
forward from a seek target for the next frame syncword, using:

    while(!found || pos == m_file_size){ ... }

That should be &&, not ||. As written, the loop can only exit via the
internal `break` when a syncword is found -- it never terminates on running
off the end of the file. If a seek lands in (or past) the final frame -- e.g.
dragging the on-screen seek bar toward the end of a track -- the search runs
past EOF, audiofile.read() returns -1 forever, that narrows to 0xFF for both
comparison bytes, the syncword check never matches, and `pos` just increments
without bound.

--- Bug 2: a falsely-matched syncword hangs the FLAC bit reader ------------

Bug 1's search only checks 2 bytes (14 matching bits) to call a position a
"syncword" -- nowhere near enough to rule out a coincidental match in real
compressed audio, and nothing downstream re-validates it. Handed a false
match (or any seek target that isn't frame-aligned), flac_decoder.cpp's
readRiceSignedInt() decodes Rice-coded residuals by reading one bit at a time
until it hits a terminating 1 bit, with no bound on how many 0 bits it will
tolerate first:

    while (readUint(1, bytesLeft) == 0)
        val++;

readUint()'s own inner loop only *logs* running out of real input
(`if(*bytesLeft < 0) log_i(...)`) and then keeps reading past the end of the
buffer anyway. So garbage/out-of-bounds bits keep flowing in, and if they
happen to read as mostly zero, this spins forever. Both fixed together: bit
reads past the available data now return 0 immediately, and the unary loop
stops as soon as data runs out instead of trusting whatever garbage bit
comes next.

Between the two: any seek -- landing on a genuinely-valid frame or not --
used to be able to hang the audio task permanently. No more commands are
processed after that (playback, transport buttons, starting new tracks all
stop responding); only UI that doesn't route through the audio task (e.g.
Back) keeps working.

--- Bug 3: a FLAC seek doesn't move the displayed elapsed time -------------

After applying a seek, processLocalFile() only updates m_audioCurrentTime
(what getAudioCurrentTime() reports, i.e. the elapsed-time/progress-bar
value) `if(m_avr_bitrate)` -- which is only ever set for MP3/WAV/M4A/AAC, not
FLAC. So on a FLAC file, dragging the seek bar forward can move playback but
leaves the displayed position exactly where it was: it looks like the seek
just didn't stick. Fixed with a fallback for when bitrate isn't available:
compute the new elapsed time as the same proportion through the duration
that the seek target is through the file's data span -- the same linear
model the app's own seek command already uses to pick that target, so the
two stay consistent.

--- Bug 4: mismatched mutex API corrupts mutex_audio, hangs Audio::loop() --

mutex_audio is created with xSemaphoreCreateMutex() (Audio.cpp ~line 154) --
a plain, non-recursive mutex. But connecttoFS() (and connecttospeech(), same
pattern) takes it with xSemaphoreTakeRecursive() and releases it with the
plain xSemaphoreGive() at every exit point. Mixing the two APIs on one handle
is invalid FreeRTOS usage: the plain give really does release the underlying
mutex, but only the recursive give decrements FreeRTOS's internal recursion
counter -- so that counter is left permanently stuck at a stale non-zero
value after every single connecttoFS() call (i.e. after every track start).

The corruption stays latent until something else takes the same mutex via
the recursive API -- which setFilePos() (the seek codepath) does. Because
the counter was already stuck above zero, that recursive take increments it
further, and its matching recursive give decrements it back without ever
reaching zero, so it never actually releases the underlying mutex. The very
next plain xSemaphoreTake(mutex_audio, portMAX_DELAY) -- the first line of
Audio::loop(), executed every single iteration -- then blocks forever
waiting for a release that can no longer happen. Confirmed against a real
device: the task watchdog firing showed CPU 0 sitting in IDLE0 (a genuine
blocked wait, not a runaway loop), and an RTC-memory checkpoint survived the
watchdog's reset showing the audio task was stuck on the very first line of
Audio::loop() -- exactly this xSemaphoreTake(mutex_audio, ...) call.

Fixed by matching every exit point's release API to its function's take API
(xSemaphoreGiveRecursive to pair with xSemaphoreTakeRecursive), in both
connecttoFS() and connecttospeech() (same bug, not currently exercised by
this project since it doesn't use TTS, but worth fixing along with it).

===============================================================================
LVGL 8.4.0
===============================================================================

--- Bug 5: lv_sjpg swallows the real TJpgDec error on decode failure -------

lv_sjpg.c's info_cb(), on the LV_IMG_SRC_VARIABLE path, calls jd_prepare()
and on any failure just sets LV_RES_INV and returns -- the actual JRESULT
error code (tjpgd.h: JDR_INTR, JDR_MEM1 "insufficient memory pool", JDR_FMT1/2/3
unsupported format, etc.) is discarded. That makes "valid-looking JPEG that
still won't decode" undiagnosable from the app side. Adds a printf of the
real rc, so a decode failure at least says why instead of just failing.
"""
Import("env")

from pathlib import Path

AUDIO_SRC = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "ESP32-audioI2S-master" / "src"
LVGL_SRC  = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "lvgl" / "src"

PATCHES = [
    (AUDIO_SRC / "Audio.cpp",
     "while(!found || pos == m_file_size){",
     "while(!found && pos < m_file_size){"),

    (AUDIO_SRC / "flac_decoder" / "flac_decoder.cpp",
     "    while (m_bitBufferLen < nBits){\n"
     "        uint8_t temp = *(m_inptr + m_rIndex);\n"
     "        m_rIndex++;\n"
     "        (*bytesLeft)--;\n"
     "        if(*bytesLeft < 0) { log_i(\"error in bitreader\"); }\n"
     "        m_bitBuffer = (m_bitBuffer << 8) | temp;\n"
     "        m_bitBufferLen += 8;\n"
     "    }",
     "    while (m_bitBufferLen < nBits){\n"
     "        if(*bytesLeft <= 0) { log_i(\"error in bitreader\"); return 0; }\n"
     "        uint8_t temp = *(m_inptr + m_rIndex);\n"
     "        m_rIndex++;\n"
     "        (*bytesLeft)--;\n"
     "        m_bitBuffer = (m_bitBuffer << 8) | temp;\n"
     "        m_bitBufferLen += 8;\n"
     "    }"),

    (AUDIO_SRC / "flac_decoder" / "flac_decoder.cpp",
     "    while (readUint(1, bytesLeft) == 0)\n"
     "        val++;",
     "    while (readUint(1, bytesLeft) == 0) {\n"
     "        val++;\n"
     "        if (*bytesLeft <= 0) break;  // bad/misaligned seek target: stop, don't spin forever\n"
     "    }"),

    (AUDIO_SRC / "flac_decoder" / "flac_decoder.cpp",
     "        while (readUint(1, bytesLeft) == 0)\n"
     "            shift++;",
     "        while (readUint(1, bytesLeft) == 0) {\n"
     "            shift++;\n"
     "            if (*bytesLeft <= 0) break;  // bad/misaligned seek target: stop, don't spin forever\n"
     "        }"),

    (AUDIO_SRC / "Audio.cpp",
     "        if(m_avr_bitrate) m_audioCurrentTime = ((m_resumeFilePos - m_audioDataStart) / m_avr_bitrate) * 8;",
     "        if(m_avr_bitrate) m_audioCurrentTime = ((m_resumeFilePos - m_audioDataStart) / m_avr_bitrate) * 8;\n"
     "        else if(m_file_size > m_audioDataStart) m_audioCurrentTime = getAudioFileDuration() *\n"
     "            (float)(m_resumeFilePos - m_audioDataStart) / (float)(m_file_size - m_audioDataStart);"),

    # connecttoFS(): 3 exit points give the plain mutex instead of the
    # recursive one it took.
    (AUDIO_SRC / "Audio.cpp",
     "    if(strlen(path)>255){\n"
     "        xSemaphoreGive(mutex_audio);\n"
     "        return false;\n"
     "    }",
     "    if(strlen(path)>255){\n"
     "        xSemaphoreGiveRecursive(mutex_audio);\n"
     "        return false;\n"
     "    }"),

    (AUDIO_SRC / "Audio.cpp",
     "    if(!audiofile) {\n"
     "        if(audio_info) {vTaskDelay(2); audio_info(\"Failed to open file for reading\");}\n"
     "        xSemaphoreGive(mutex_audio);\n"
     "        return false;\n"
     "    }",
     "    if(!audiofile) {\n"
     "        if(audio_info) {vTaskDelay(2); audio_info(\"Failed to open file for reading\");}\n"
     "        xSemaphoreGiveRecursive(mutex_audio);\n"
     "        return false;\n"
     "    }"),

    (AUDIO_SRC / "Audio.cpp",
     "    bool ret = initializeDecoder();\n"
     "    if(ret) m_f_running = true;\n"
     "    else audiofile.close();\n"
     "    xSemaphoreGive(mutex_audio);\n"
     "    return ret;\n"
     "}",
     "    bool ret = initializeDecoder();\n"
     "    if(ret) m_f_running = true;\n"
     "    else audiofile.close();\n"
     "    xSemaphoreGiveRecursive(mutex_audio);\n"
     "    return ret;\n"
     "}"),

    # connecttospeech(): same bug, same fix. Not used by this project (no
    # TTS), fixed anyway since it's the identical pattern in the same file.
    (AUDIO_SRC / "Audio.cpp",
     "    if(!speechBuff) {\n"
     "        log_e(\"out of memory\");\n"
     "        xSemaphoreGive(mutex_audio);\n"
     "        return false;\n"
     "    }",
     "    if(!speechBuff) {\n"
     "        log_e(\"out of memory\");\n"
     "        xSemaphoreGiveRecursive(mutex_audio);\n"
     "        return false;\n"
     "    }"),

    (AUDIO_SRC / "Audio.cpp",
     "    if(!_client->connect(host, 80)) {\n"
     "        log_e(\"Connection failed\");\n"
     "        xSemaphoreGive(mutex_audio);\n"
     "        return false;\n"
     "    }",
     "    if(!_client->connect(host, 80)) {\n"
     "        log_e(\"Connection failed\");\n"
     "        xSemaphoreGiveRecursive(mutex_audio);\n"
     "        return false;\n"
     "    }"),

    (AUDIO_SRC / "Audio.cpp",
     "    m_f_tts = true;\n"
     "    setDatamode(HTTP_RESPONSE_HEADER);\n"
     "    xSemaphoreGive(mutex_audio);\n"
     "    return true;\n"
     "}",
     "    m_f_tts = true;\n"
     "    setDatamode(HTTP_RESPONSE_HEADER);\n"
     "    xSemaphoreGiveRecursive(mutex_audio);\n"
     "    return true;\n"
     "}"),

    # lv_sjpg.c: surface the real TJpgDec error code instead of discarding it.
    (LVGL_SRC / "extra" / "libs" / "sjpg" / "lv_sjpg.c",
     "#include \"tjpgd.h\"\n"
     "#include \"lv_sjpg.h\"\n"
     "#include \"../../../misc/lv_fs.h\"",
     "#include \"tjpgd.h\"\n"
     "#include \"lv_sjpg.h\"\n"
     "#include \"../../../misc/lv_fs.h\"\n"
     "#include <stdio.h>"),

    (LVGL_SRC / "extra" / "libs" / "sjpg" / "lv_sjpg.c",
     "            JRESULT rc = jd_prepare(&jd_tmp, input_func, workb_temp, (size_t)TJPGD_WORKBUFF_SIZE, &io_source_temp);\n"
     "            if(rc == JDR_OK) {\n"
     "                header->w = jd_tmp.width;\n"
     "                header->h = jd_tmp.height;\n"
     "\n"
     "            }\n"
     "            else {\n"
     "                ret = LV_RES_INV;\n"
     "                goto end;\n"
     "            }\n"
     "\n"
     "end:\n"
     "            lv_mem_free(workb_temp);\n"
     "\n"
     "            return ret;\n"
     "\n"
     "        }\n"
     "    }\n"
     "    else if(src_type == LV_IMG_SRC_FILE) {",
     "            JRESULT rc = jd_prepare(&jd_tmp, input_func, workb_temp, (size_t)TJPGD_WORKBUFF_SIZE, &io_source_temp);\n"
     "            if(rc == JDR_OK) {\n"
     "                header->w = jd_tmp.width;\n"
     "                header->h = jd_tmp.height;\n"
     "\n"
     "            }\n"
     "            else {\n"
     "                printf(\"[art] TJpgDec jd_prepare failed: rc=%d data_size=%u workbuf=%u"
     " (see tjpgd.h JRESULT enum for rc meaning)\\n\",\n"
     "                       (int)rc, (unsigned)raw_sjpeg_data_size, (unsigned)TJPGD_WORKBUFF_SIZE);\n"
     "                ret = LV_RES_INV;\n"
     "                goto end;\n"
     "            }\n"
     "\n"
     "end:\n"
     "            lv_mem_free(workb_temp);\n"
     "\n"
     "            return ret;\n"
     "\n"
     "        }\n"
     "    }\n"
     "    else if(src_type == LV_IMG_SRC_FILE) {"),

    # tjpgd.c: accept 4:4:0 (1x2) chroma subsampling.
    #
    # TJpgDec rejects a Y sampling factor of 0x12 outright, which is why some
    # perfectly ordinary baseline album art decodes to nothing (rc=8, JDR_FMT3).
    # Nothing about 1x2 is actually beyond the decoder -- the block loading,
    # the buffer sizing and the IDCT are all written in terms of msx/msy and
    # handle it as-is. Only two lines in mcu_output() are hardcoded for the
    # 2x2 case, and both are fixed below, so the check can be relaxed.
    (LVGL_SRC / "extra" / "libs" / "sjpg" / "tjpgd.c",
     "\t\t\t\t\tif (b != 0x11 && b != 0x22 && b != 0x21) {\t/* Check sampling factor */\n"
     "\t\t\t\t\t\treturn JDR_FMT3;\t\t\t\t\t/* Err: Supports only 4:4:4, 4:2:0 or 4:2:2 */",
     "\t\t\t\t\tif (b != 0x11 && b != 0x22 && b != 0x21 && b != 0x12) {\t/* Check sampling factor */\n"
     "\t\t\t\t\t\treturn JDR_FMT3;\t\t\t\t\t/* Err: Supports only 4:4:4, 4:2:0, 4:2:2 or 4:4:0 */"),

    # The two hardcoded-for-2x2 lines, in mcu_output()'s "double block height"
    # branch (which now means msy==2 with msx either 1 or 2, not just 2x2):
    #
    #   chroma base: the C blocks sit after the Y blocks, so the offset is
    #   msx*msy blocks in, not the literal 4 that a 2x2 MCU happens to have.
    #
    #   luma second row: the block row below starts msx blocks in. The old
    #   constant 64 is right only when msx is 2, because "iy * 8" below
    #   already contributes the other 64. For msx==1 it has to add nothing.
    #
    # Both reduce to exactly the original values when msx==msy==2, so 4:2:0,
    # 4:2:2 and 4:4:4 decode byte-for-byte as before.
    (LVGL_SRC / "extra" / "libs" / "sjpg" / "tjpgd.c",
     "\t\t\t\t\tpc += 64 * 4 + (iy >> 1) * 8;\n"
     "\t\t\t\t\tif (iy >= 8) py += 64;",
     "\t\t\t\t\tpc += jd->msx * jd->msy * 64 + (iy >> 1) * 8;\n"
     "\t\t\t\t\tif (iy >= 8) py += (jd->msx - 1) * 64;"),
]


def patch():
    touched = 0
    for path, buggy, fixed in PATCHES:
        if not path.exists():
            continue  # not fetched yet; this hook runs again on the next `pio run`

        text = path.read_text()
        count = text.count(buggy)
        if count == 0:
            continue  # already patched, or the library's source has changed shape

        path.write_text(text.replace(buggy, fixed))
        print(f"[patch_libs] fixed {count} bug(s) in {path.name}: {buggy.strip()[:60]}...")
        touched += count

    if touched:
        print(f"[patch_libs] applied {touched} fix(es) total")


patch()
