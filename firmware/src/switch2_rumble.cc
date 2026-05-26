#include "switch2_rumble.h"

#include <algorithm>
#include <cstring>

static const uint16_t SWITCH2_RUMBLE_MAX = 29000;

static uint16_t decode_high_amp(const uint8_t* data) {
    return (((uint16_t) data[3] & 0xfc) << 4) | (((uint16_t) data[4] & 0x0f) << 12);
}

static uint16_t decode_low_amp(const uint8_t* data) {
    return ((uint16_t) data[5] & 0xc0) | ((uint16_t) data[6] << 8);
}

static uint8_t amp_to_xbox(uint16_t amp) {
    amp = std::min(amp, SWITCH2_RUMBLE_MAX);
    return (uint8_t) ((uint32_t) amp * 255 / SWITCH2_RUMBLE_MAX);
}

bool switch2_rumble_decode(uint8_t report_id, const uint8_t* buffer, uint16_t len, uint8_t* low_frequency, uint8_t* high_frequency) {
    uint8_t report[64] = { 0 };
    uint16_t report_len = 0;

    if (report_id == 0) {
        report_len = std::min<uint16_t>(len, sizeof(report));
        memcpy(report, buffer, report_len);
    } else {
        report[0] = report_id;
        uint16_t copy_len = std::min<uint16_t>(len, sizeof(report) - 1);
        memcpy(report + 1, buffer, copy_len);
        report_len = copy_len + 1;
    }

    if (report_len < 7 ||
        (report[0] != 0x01 && report[0] != 0x02 && report[0] != 0x10)) {
        return false;
    }

    uint16_t low_amp = decode_low_amp(report);
    uint16_t high_amp = decode_high_amp(report);

    if (report_len >= 23) {
        low_amp = std::max(low_amp, decode_low_amp(report + 16));
        high_amp = std::max(high_amp, decode_high_amp(report + 16));
    }

    *low_frequency = amp_to_xbox(low_amp);
    *high_frequency = amp_to_xbox(high_amp);
    return true;
}
