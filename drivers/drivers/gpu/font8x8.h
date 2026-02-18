/*
 * font8x8.h - 8x8 bitmap font (Basic Latin + Latin-1)
 * Each char is 8 rows of 8 bits.  Bit 7 of each byte = leftmost pixel.
 */
#ifndef FONT8X8_H
#define FONT8X8_H

#include <stdint.h>

/* 128-character ASCII font, 8 bytes per glyph */
extern const uint8_t font8x8_basic[128][8];

#endif /* FONT8X8_H */
