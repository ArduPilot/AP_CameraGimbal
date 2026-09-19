#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "apcam/target.h"
/* ZR10 uses native controls and the SIYI MCU query tunnel. */
#include <stdint.h>
extern int ca_zr10_take_focus(uint8_t payload[27]) __attribute__((weak));
#include "../siyi/control.cpp"
