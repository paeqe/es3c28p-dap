#pragma once
#include <Arduino.h>

namespace tonetest {

// Plays a sine sweep straight out of I2S, bypassing the SD card and the
// decoders. Blocks for roughly six seconds.
//
// If this is silent, the problem is the ES8311 configuration or the amplifier.
// If it plays but files do not, the problem is the card or the decoder.
void run();

// The same tone, but driven straight at the external PCM510x DAC's own pins,
// on a private I2S driver this code controls end to end. It touches neither
// the audio library, the runtime pin re-routing in player.cpp, nor the ES8311.
//
// This is the test that splits the problem in half. If you hear this but the
// HP button gives you nothing, the module and its wiring are fine and the
// fault is in the firmware's output switching. If this is silent too, it is
// the wiring or the module's own configuration -- check SCK is grounded, then
// XSMT (must be high) and FMT (must be low).
void runDac();

} // namespace tonetest
