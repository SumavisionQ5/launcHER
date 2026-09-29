#ifndef LAUNCHER_CN_SCREEN_H
#define LAUNCHER_CN_SCREEN_H

#include <stdarg.h>

/* Initialise the launcHER UTF-8 aware debug screen. */
void cn_screen_init(void);

/* Print UTF-8 text using the original PS2SDK 8x8 font for ASCII and
 * an embedded/generated 16x16 bitmap font for BMP CJK characters. */
void cn_screen_vprintf(const char *format, va_list args);

#endif
