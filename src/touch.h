#pragma once
#include <Arduino.h>

namespace touch {

// Assumes Wire.begin() has already been called.
bool begin();

// Returns true when a finger is down; writes screen coordinates with the
// axis transform applied.
bool read(int32_t &x, int32_t &y);

// Untransformed controller values, for calibration.
bool readRaw(int32_t &x, int32_t &y, uint8_t &fingers);

// Axis correction applied by read(). Defaults are all false.
void setTransform(bool swapXY, bool invertX, bool invertY);

// Prints every responding address on the I2C bus. Useful when the codec or
// the touch controller goes missing.
void scanBus();

} // namespace touch
