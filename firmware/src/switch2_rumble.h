#ifndef _SWITCH2_RUMBLE_H_
#define _SWITCH2_RUMBLE_H_

#include <stdint.h>

bool switch2_rumble_decode(uint8_t report_id, const uint8_t* buffer, uint16_t len, uint8_t* low_frequency, uint8_t* high_frequency);

#endif
