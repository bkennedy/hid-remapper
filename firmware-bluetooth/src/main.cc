#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/hogp.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/types.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

#include "config.h"
#include "descriptor_parser.h"
#include "globals.h"
#include "our_descriptor.h"
#include "platform.h"
#include "remapper.h"

LOG_MODULE_REGISTER(remapper, LOG_LEVEL_DBG);

#define CHK(X) ({ int err = X; if (err != 0) { LOG_ERR("%s returned %d (%s:%d)", #X, err, __FILE__, __LINE__); } err == 0; })

static const int SCAN_DELAY_MS = 1000;
static const int CLEAR_BONDS_BUTTON_PRESS_MS = 3000;

// these macros don't work in C++ when used directly ("taking address of temporary array")
static auto const BT_UUID_HIDS_ = (struct bt_uuid_16) BT_UUID_INIT_16(BT_UUID_HIDS_VAL);
static auto BT_ADDR_LE_ANY_ = BT_ADDR_LE_ANY[0];
static auto BT_CONN_LE_CREATE_CONN_ = BT_CONN_LE_CREATE_CONN[0];

static struct bt_hogp hogps[CONFIG_BT_MAX_CONN];

static K_SEM_DEFINE(usb_sem0, 1, 1);
static K_SEM_DEFINE(usb_sem1, 1, 1);
static K_SEM_DEFINE(switch2_vendor_in_sem, 1, 1);

// Auth relay: Mac Python script reads "AUTH02:...\n" from serial, sends to real
// SW2 Pro USB, writes "R02:...\n" back. Firmware busy-polls uart_poll_in up to
// 2000ms while waiting. Only sub=0x02 is relayed; sub=0x04 is constant.

// Read up to timeout_ms waiting for "R02:<32 hex chars>\n" from relay script.
// Returns true and fills out[16] if received; false on timeout.
static bool sw2_wait_relay_response(uint8_t out[16], uint32_t timeout_ms) {
    const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint8_t buf[68];
    int pos = 0;
    int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline) {
        uint8_t c;
        if (uart_poll_in(uart, &c) == 0) {
            buf[pos++] = c;
            if (c == '\n') {
                if (pos >= 36 && buf[0]=='R' && buf[1]=='0' && buf[2]=='2' && buf[3]==':') {
                    // Parse 16 hex bytes
                    bool ok = true;
                    for (int i = 0; i < 16 && ok; i++) {
                        char h[3] = { (char)buf[4+i*2], (char)buf[5+i*2], '\0' };
                        char *end;
                        long val = strtol(h, &end, 16);
                        if (end == h) { ok = false; break; }
                        out[i] = (uint8_t)val;
                    }
                    if (ok) return true;
                }
                pos = 0;
            }
            if (pos >= (int)sizeof(buf)) pos = 0;
        } else {
            k_busy_wait(500);  // 0.5ms
        }
    }
    return false;
}

static struct k_mutex mutexes[(uint8_t) MutexId::N];

static struct k_mutex get_report_mutex;
static uint8_t get_report_buf[64];
static bool get_report_response_ready = false;

static const struct device* hid_dev0;
static const struct device* hid_dev1;  // config interface

#define SWITCH2_VENDOR_OUT_EP 0x02
#define SWITCH2_VENDOR_IN_EP 0x82
#define SWITCH2_VENDOR_EP_MPS 64

struct report_type {
    uint16_t interface;
    uint8_t len;
    uint8_t data[65];
};

struct descriptor_type {
    uint16_t size;
    uint8_t conn_idx;
    uint8_t data[512];
};

struct hogp_ready_type {
    struct bt_hogp* hogp;
};

struct disconnected_type {
    uint8_t conn_idx;
};

struct set_report_type {
    uint8_t report_id;
    uint8_t interface;
    uint16_t len;
    uint8_t data[64];
};

K_MSGQ_DEFINE(report_q, sizeof(struct report_type), 16, 4);
K_MSGQ_DEFINE(descriptor_q, sizeof(struct descriptor_type), 2, 4);
K_MSGQ_DEFINE(hogp_ready_q, sizeof(struct hogp_ready_type), CONFIG_BT_MAX_CONN, 4);
K_MSGQ_DEFINE(disconnected_q, sizeof(struct disconnected_type), CONFIG_BT_MAX_CONN, 4);
K_MSGQ_DEFINE(set_report_q, sizeof(struct set_report_type), 8, 4);
K_MSGQ_DEFINE(switch_pro_response_q, 65, 16, 4);  // byte[0]=len, bytes[1..64]=data
ATOMIC_DEFINE(tick_pending, 1);

enum class ConnKind : uint8_t {
    NONE = 0,
    HOGP = 1,
    SWITCH2_PRO = 2,
};

enum class Switch2ProInitState : uint8_t {
    IDLE = 0,
    READ_INFO,
    PAIR_STEP1,
    PAIR_STEP2,
    PAIR_STEP3,
    PAIR_STEP4,
    SET_LED,
    DONE,
};

struct switch2_pro_client {
    struct bt_conn* conn;
    struct bt_gatt_subscribe_params input_subscribe_params;
    struct bt_gatt_subscribe_params ack_subscribe_params;
    struct bt_gatt_exchange_params mtu_exchange_params;
    uint16_t input_value_handle;
    uint16_t output_value_handle;
    uint16_t cmd_value_handle;
    uint8_t conn_idx;
    Switch2ProInitState init_state;
    bool init_cmd_in_flight;
    int64_t init_cmd_sent_at;
};

static ConnKind conn_kinds[CONFIG_BT_MAX_CONN];
static bt_addr_le_t pending_switch2_pro_addr;
static bool pending_switch2_pro_valid = false;
static switch2_pro_client switch2_pro_clients[CONFIG_BT_MAX_CONN];

#define SW0_NODE DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS(SW0_NODE, okay)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif

#define LED0_NODE DT_ALIAS(led0)
#if !DT_NODE_HAS_STATUS(LED0_NODE, okay)
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

#define LED1_NODE DT_ALIAS(led1)
#if !DT_NODE_HAS_STATUS(LED1_NODE, okay)
#error "Unsupported board: led1 devicetree alias is not defined"
#endif

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);

static struct gpio_callback button_cb_data;

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);

static bool scanning = false;
static bool peers_only = true;

static struct bt_le_conn_param* conn_param = BT_LE_CONN_PARAM(6, 6, 44, 400);

static bool switch_pro_input_enabled = false;
static uint8_t switch_pro_timer = 0;
static int64_t switch_pro_last_input_ms = 0;
static int64_t switch_pro_last_stats_ms = 0;
static int64_t switch_pro_last_button_log_ms = 0;
static uint8_t switch_pro_current_input[64];

static uint32_t switch_pro_ble_reports = 0;
static uint32_t switch_pro_ble_report_drops = 0;
static uint32_t switch_pro_host_reports = 0;
static uint32_t switch_pro_host_report_drops = 0;
static uint32_t switch_pro_set_reports = 0;
static uint32_t switch_pro_translated_reports = 0;
static uint32_t switch_pro_mapped_writes = 0;
static uint32_t switch_pro_mapped_write_fails = 0;
static uint32_t switch_pro_heartbeat_writes = 0;
static uint32_t switch_pro_heartbeat_write_fails = 0;
static uint32_t switch_pro_response_writes = 0;
static uint32_t switch_pro_response_write_fails = 0;
static uint32_t switch_pro_response_drops = 0;
static uint32_t switch_pro_report_q_highwater = 0;
static uint32_t switch_pro_response_q_highwater = 0;
static uint32_t switch_pro_bt_connected_events = 0;
static int64_t sw2_lr_autopress_ms = 0;  // start time for L+R auto-press; 0=not started
static uint32_t switch_pro_bt_disconnected_events = 0;
static uint32_t switch_pro_hogp_ready_events = 0;
static uint32_t switch_pro_conn_count_highwater = 0;
static uint32_t switch_pro_last_disconnect_reason = 0;
static uint32_t switch_pro_rumble_reports = 0;
static uint32_t switch_pro_rumble_writes = 0;
static uint32_t switch_pro_rumble_write_fails = 0;
static uint32_t switch_pro_rumble_busy = 0;
static uint8_t switch_pro_last_xbox_rumble[8] = {};
static int64_t switch_pro_last_xbox_rumble_ms = 0;

#define SWITCH_PRO_DIAG_PAGES 5
#define SWITCH_PRO_DIAG_VALUES 7

static uint32_t switch_pro_saved_diag[SWITCH_PRO_DIAG_PAGES][SWITCH_PRO_DIAG_VALUES];
static int64_t switch_pro_last_diag_persist_ms = 0;
static uint16_t switch_pro_axis_last[4] = { 0x8000, 0x8000, 0x8000, 0x8000 };
static uint16_t switch_pro_axis_min[4] = { 0xffff, 0xffff, 0xffff, 0xffff };
static uint16_t switch_pro_axis_max[4] = { 0, 0, 0, 0 };

#define SWITCH2_FLIGHT_MAGIC 0x32574648  // HFW2
#define SWITCH2_FLIGHT_VERSION 1
#define SWITCH2_FLIGHT_EVENTS 64
#define SWITCH2_FLIGHT_DATA_LEN 8
#define SWITCH2_BOND_KEYS_MAGIC 0x324b4253  // SBK2
#define SWITCH2_BOND_KEYS_VERSION 1
#define SWITCH2_BOND_KEYS_DATA_LEN 1024

enum class Switch2FlightEvent : uint8_t {
    BOOT = 1,
    USB_STATUS = 2,
    SET_REPORT = 3,
    GET_REPORT = 4,
    INT_OUT = 5,
    HOST_CMD = 6,
    QUEUE_RESPONSE = 7,
    SEND_RESPONSE = 8,
    SEND_INPUT = 9,
    INPUT_ENABLE = 10,
    BLE_INPUT = 11,
    BLE_INIT = 12,
    RUMBLE = 13,
    CONFIG_SET = 14,
    BT_CONNECT = 15,
    BT_DISCONNECT = 16,
    BT_SECURITY = 17,
    BT_PAIRING = 18,
    SCAN = 19,
    BOND_KEYS = 20,
};

struct switch2_flight_event {
    uint32_t ms;
    uint16_t seq;
    uint8_t event;
    uint8_t len;
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t d;
    uint8_t data[SWITCH2_FLIGHT_DATA_LEN];
};

struct switch2_flight_log {
    uint32_t magic;
    uint16_t version;
    uint16_t next_seq;
    uint8_t head;
    uint8_t wrapped;
    uint8_t reserved[2];
    switch2_flight_event events[SWITCH2_FLIGHT_EVENTS];
};

struct switch2_bond_keys_snapshot {
    uint32_t magic;
    uint16_t version;
    uint16_t total_len;
    uint8_t record_count;
    uint8_t truncated;
    uint8_t reserved[2];
    uint8_t data[SWITCH2_BOND_KEYS_DATA_LEN];
};

static switch2_flight_log switch2_flight = {
    .magic = SWITCH2_FLIGHT_MAGIC,
    .version = SWITCH2_FLIGHT_VERSION,
};
static switch2_bond_keys_snapshot switch2_bond_keys = {
    .magic = SWITCH2_BOND_KEYS_MAGIC,
    .version = SWITCH2_BOND_KEYS_VERSION,
};
static bool switch2_flight_dirty = false;
static int64_t switch2_flight_last_persist_ms = 0;
static int64_t switch2_flight_last_input_event_ms = 0;
static bool switch2_pro_ble_enabled = false;

static bool is_switch1_pro_mode() {
    return our_descriptor_number == 6;
}

static bool is_switch2_pro_mode() {
    return our_descriptor_number == 7;
}

static bool is_switch2_pro_ble_mode() {
    return is_switch2_pro_mode() || switch2_pro_ble_enabled;
}

static bool is_switch_pro_mode() {
    return is_switch1_pro_mode() || is_switch2_pro_mode();
}

static void switch2_flight_reset_if_invalid() {
    if (switch2_flight.magic == SWITCH2_FLIGHT_MAGIC &&
        switch2_flight.version == SWITCH2_FLIGHT_VERSION &&
        switch2_flight.head < SWITCH2_FLIGHT_EVENTS) {
        return;
    }

    memset(&switch2_flight, 0, sizeof(switch2_flight));
    switch2_flight.magic = SWITCH2_FLIGHT_MAGIC;
    switch2_flight.version = SWITCH2_FLIGHT_VERSION;
}

static void switch2_flight_record(Switch2FlightEvent event, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0, const uint8_t* data = NULL, uint8_t len = 0) {
    if (!is_switch2_pro_ble_mode() && event != Switch2FlightEvent::BOOT && event != Switch2FlightEvent::CONFIG_SET) {
        return;
    }

    switch2_flight_reset_if_invalid();

    switch2_flight_event* item = &switch2_flight.events[switch2_flight.head];
    memset(item, 0, sizeof(*item));
    item->ms = (uint32_t) k_uptime_get_32();
    item->seq = switch2_flight.next_seq++;
    item->event = (uint8_t) event;
    item->a = a;
    item->b = b;
    item->c = c;
    item->d = d;
    item->len = MIN(len, (uint8_t) SWITCH2_FLIGHT_DATA_LEN);
    if (data && item->len) {
        memcpy(item->data, data, item->len);
    }

    switch2_flight.head++;
    if (switch2_flight.head >= SWITCH2_FLIGHT_EVENTS) {
        switch2_flight.head = 0;
        switch2_flight.wrapped = 1;
    }
    switch2_flight_dirty = true;
}

static const char* switch2_flight_event_name(uint8_t event) {
    switch ((Switch2FlightEvent) event) {
        case Switch2FlightEvent::BOOT: return "boot";
        case Switch2FlightEvent::USB_STATUS: return "usb_status";
        case Switch2FlightEvent::SET_REPORT: return "set_report";
        case Switch2FlightEvent::GET_REPORT: return "get_report";
        case Switch2FlightEvent::INT_OUT: return "int_out";
        case Switch2FlightEvent::HOST_CMD: return "host_cmd";
        case Switch2FlightEvent::QUEUE_RESPONSE: return "queue_response";
        case Switch2FlightEvent::SEND_RESPONSE: return "send_response";
        case Switch2FlightEvent::SEND_INPUT: return "send_input";
        case Switch2FlightEvent::INPUT_ENABLE: return "input_enable";
        case Switch2FlightEvent::BLE_INPUT: return "ble_input";
        case Switch2FlightEvent::BLE_INIT: return "ble_init";
        case Switch2FlightEvent::RUMBLE: return "rumble";
        case Switch2FlightEvent::CONFIG_SET: return "config_set";
        case Switch2FlightEvent::BT_CONNECT: return "bt_connect";
        case Switch2FlightEvent::BT_DISCONNECT: return "bt_disconnect";
        case Switch2FlightEvent::BT_SECURITY: return "bt_security";
        case Switch2FlightEvent::BT_PAIRING: return "bt_pairing";
        case Switch2FlightEvent::SCAN: return "scan";
        case Switch2FlightEvent::BOND_KEYS: return "bond_keys";
        default: return "unknown";
    }
}

static void switch2_bond_keys_reset() {
    memset(&switch2_bond_keys, 0, sizeof(switch2_bond_keys));
    switch2_bond_keys.magic = SWITCH2_BOND_KEYS_MAGIC;
    switch2_bond_keys.version = SWITCH2_BOND_KEYS_VERSION;
}

static void switch2_bond_keys_append(const char* name, const uint8_t* value, uint16_t value_len) {
    uint8_t name_len = (uint8_t) MIN(strlen(name), (size_t) 63);
    uint32_t needed = 1 + 2 + name_len + value_len;
    uint32_t remaining = SWITCH2_BOND_KEYS_DATA_LEN - switch2_bond_keys.total_len;

    if (needed > remaining) {
        switch2_bond_keys.truncated = 1;
        return;
    }

    uint8_t* out = switch2_bond_keys.data + switch2_bond_keys.total_len;
    *out++ = name_len;
    sys_put_le16(value_len, out);
    out += 2;
    memcpy(out, name, name_len);
    out += name_len;
    memcpy(out, value, value_len);
    switch2_bond_keys.total_len += needed;
    switch2_bond_keys.record_count++;
}

static int switch2_bond_keys_settings_cb(const char* name, size_t len, settings_read_cb read_cb, void* cb_arg, void* param) {
    ARG_UNUSED(param);

    if (!name || len == 0) {
        return 0;
    }

    if (len > 255) {
        switch2_bond_keys.truncated = 1;
        len = 255;
    }

    uint8_t value[255];
    int bytes_read = read_cb(cb_arg, value, len);
    if (bytes_read < 0) {
        switch2_bond_keys.truncated = 1;
        return bytes_read;
    }

    switch2_bond_keys_append(name, value, (uint16_t) bytes_read);
    return 0;
}

static void switch2_bond_keys_snapshot_now() {
    switch2_bond_keys_reset();
    int err = settings_load_subtree_direct("bt/keys", switch2_bond_keys_settings_cb, NULL);
    switch2_flight_record(Switch2FlightEvent::BOND_KEYS,
        err ? 0xff : switch2_bond_keys.record_count,
        switch2_bond_keys.truncated,
        (uint8_t) (switch2_bond_keys.total_len & 0xff),
        (uint8_t) (switch2_bond_keys.total_len >> 8));
}

static void switch2_flight_dump_saved() {
    switch2_flight_reset_if_invalid();

    uint8_t count = switch2_flight.wrapped ? SWITCH2_FLIGHT_EVENTS : switch2_flight.head;
    uint8_t start = switch2_flight.wrapped ? switch2_flight.head : 0;
    LOG_INF("switch2_flight dump count=%u next_seq=%u wrapped=%u", count, switch2_flight.next_seq, switch2_flight.wrapped);
    for (uint8_t i = 0; i < count; i++) {
        const switch2_flight_event* item = &switch2_flight.events[(start + i) % SWITCH2_FLIGHT_EVENTS];
        LOG_INF("switch2_flight seq=%u ms=%u event=%s(%u) a=%02x b=%02x c=%02x d=%02x len=%u data=%02x %02x %02x %02x %02x %02x %02x %02x",
            item->seq, item->ms, switch2_flight_event_name(item->event), item->event,
            item->a, item->b, item->c, item->d, item->len,
            item->data[0], item->data[1], item->data[2], item->data[3],
            item->data[4], item->data[5], item->data[6], item->data[7]);
    }
}

static void switch2_flight_persist(bool force = false) {
    if (!switch2_flight_dirty) {
        return;
    }

    int64_t now = k_uptime_get();
    if (!force && switch2_flight_last_persist_ms && (now - switch2_flight_last_persist_ms < 750)) {
        return;
    }

    switch2_flight_last_persist_ms = now;
    switch2_flight_dirty = false;
    settings_save_one("remapper/switch2_flight", &switch2_flight, sizeof(switch2_flight));
}

static void switch2_flight_record_send_input(bool sent) {
    if (!is_switch2_pro_mode()) {
        return;
    }

    int64_t now = k_uptime_get();
    if (sent && switch2_flight_last_input_event_ms && (now - switch2_flight_last_input_event_ms < 1000)) {
        return;
    }
    switch2_flight_last_input_event_ms = now;
    switch2_flight_record(Switch2FlightEvent::SEND_INPUT, switch_pro_current_input[0], sent, switch_pro_input_enabled, switch_pro_current_input[1], switch_pro_current_input, sizeof(switch_pro_current_input));
}

static void switch_pro_reset_input() {
    memset(switch_pro_current_input, 0, sizeof(switch_pro_current_input));
    if (is_switch2_pro_mode()) {
        switch_pro_current_input[0] = 0x09;
        switch_pro_current_input[2] = 0x20;  // USB connected, battery OK (real capture: 0x20)
        switch_pro_current_input[6] = 0x00;
        switch_pro_current_input[7] = 0x08;
        switch_pro_current_input[8] = 0x80;
        switch_pro_current_input[9] = 0x00;
        switch_pro_current_input[10] = 0x08;
        switch_pro_current_input[11] = 0x80;
        switch_pro_current_input[12] = 0x30;  // transitions to 0x00 after cmd=0x0c:0x04
        // Placeholder IMU data (bytes 14-44); replace with live BLE values later
        static const uint8_t sw2_imu_placeholder[31] = {
            0x1e, 0x87, 0x44, 0x00, 0x0c, 0x00, 0xfc, 0x25,
            0x01, 0x02, 0xef, 0xa2, 0xfe, 0x00, 0x6a, 0xd5,
            0x7f, 0x54, 0x59, 0x9e, 0x02, 0x78, 0x69, 0x25,
            0xf8, 0x30, 0xb7, 0xd0, 0x0d, 0x71, 0x00,
        };
        memcpy(switch_pro_current_input + 14, sw2_imu_placeholder, sizeof(sw2_imu_placeholder));
        return;
    }
    switch_pro_current_input[0] = 0x30;
    switch_pro_current_input[2] = 0x91;
    switch_pro_current_input[4] = 0x80;
    switch_pro_current_input[6] = 0x00;
    switch_pro_current_input[7] = 0x08;
    switch_pro_current_input[8] = 0x80;
    switch_pro_current_input[9] = 0x00;
    switch_pro_current_input[10] = 0x08;
    switch_pro_current_input[11] = 0x80;
    switch_pro_current_input[12] = 0x0c;
}

static void switch_pro_reset_axis_diagnostics() {
    for (uint8_t axis = 0; axis < 4; axis++) {
        switch_pro_axis_last[axis] = 0x8000;
        switch_pro_axis_min[axis] = 0xffff;
        switch_pro_axis_max[axis] = 0;
    }
}

static void switch_pro_reset_session() {
    switch_pro_input_enabled = false;
    switch_pro_timer = 0;
    switch_pro_last_input_ms = 0;
    switch_pro_last_stats_ms = 0;
    switch_pro_last_button_log_ms = 0;
    sw2_lr_autopress_ms = 0;  // reset per-session so L+R fires fresh after each USB CONFIGURE
    k_msgq_purge(&switch_pro_response_q);
    switch_pro_reset_input();
    switch_pro_reset_axis_diagnostics();
}

static uint32_t queue_depth(struct k_msgq* queue) {
    return k_msgq_num_used_get(queue);
}

static void note_switch_pro_report_q_depth() {
    uint32_t depth = queue_depth(&report_q);
    if (depth > switch_pro_report_q_highwater) {
        switch_pro_report_q_highwater = depth;
    }
}

static void note_switch_pro_response_q_depth() {
    uint32_t depth = queue_depth(&switch_pro_response_q);
    if (depth > switch_pro_response_q_highwater) {
        switch_pro_response_q_highwater = depth;
    }
}

static bool report_button_pressed(const uint8_t* report, size_t button_index) {
    const size_t bit = button_index - 1;
    return (report[1 + bit / 8] & BIT(bit % 8)) != 0;
}

static uint16_t report_axis_16(const uint8_t* report, size_t offset) {
    return (uint16_t) report[offset] | ((uint16_t) report[offset + 1] << 8);
}

static uint16_t switch_pro_apply_axis_deadzone(uint16_t value) {
    const uint16_t center = 0x8000;
    const uint16_t threshold = 0x0100;

    if (value >= center - threshold && value <= center + threshold) {
        return center;
    }
    return value;
}

static uint16_t switch_pro_expand_axis(uint16_t value) {
    if (value <= 0x00ff) {
        return value * 0x0101;
    }
    return value;
}

static uint16_t switch_pro_invert_axis(uint16_t value) {
    return 0xffff - value;
}

static uint16_t switch_pro_normalize_axis(uint16_t value, bool invert) {
    value = switch_pro_expand_axis(value);
    if (invert) {
        value = switch_pro_invert_axis(value);
    }
    return switch_pro_apply_axis_deadzone(value);
}

static uint16_t switch_pro_axis_from_state_or_report(uint32_t usage, uint16_t report_value) {
    bool found = false;
    int32_t value = get_input_state_value(usage, 0, false, &found);
    if (!found) {
        return report_value;
    }
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        value = 255;
    }
    return (uint16_t) (value * 0x0101);
}

static void switch_pro_note_axis(uint8_t axis, uint16_t value) {
    switch_pro_axis_last[axis] = value;
    if (value < switch_pro_axis_min[axis]) {
        switch_pro_axis_min[axis] = value;
    }
    if (value > switch_pro_axis_max[axis]) {
        switch_pro_axis_max[axis] = value;
    }
}

static uint16_t switch_pro_clamp_axis_range(uint16_t value) {
    const uint16_t min = 0x2000;
    const uint16_t max = 0xe000;

    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static void pack_switch_pro_stick(uint8_t* out, uint16_t x16, uint16_t y16) {
    /*
     * Report 0x30 uses packed 12-bit stick positions. Values passed here are
     * normalized to the 16-bit HID Remapper output range.
     */
    x16 = switch_pro_clamp_axis_range(x16);
    y16 = switch_pro_clamp_axis_range(y16);
    uint16_t x = x16 >> 4;
    uint16_t y = y16 >> 4;

    out[0] = x & 0xff;
    out[1] = ((x >> 8) & 0x0f) | ((y & 0x0f) << 4);
    out[2] = (y >> 4) & 0xff;
}

static uint16_t switch2_pro_axis12_from_state_or_report(uint32_t usage, uint16_t report_value) {
    bool found = false;
    int32_t value = get_input_state_value(usage, 0, false, &found);
    if (!found) {
        return report_value & 0x0fff;
    }
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        value = 255;
    }
    return (uint16_t) ((value * 0x0fff) / 255);
}

static void pack_switch2_pro_stick12(uint8_t* out, uint16_t x, uint16_t y) {
    x &= 0x0fff;
    y &= 0x0fff;
    out[0] = x & 0xff;
    out[1] = ((x >> 8) & 0x0f) | ((y & 0x0f) << 4);
    out[2] = (y >> 4) & 0xff;
}

static uint16_t switch2_pro_unpack_stick_x(const uint8_t* data) {
    return (uint16_t) data[0] | (((uint16_t) data[1] & 0x0f) << 8);
}

static uint16_t switch2_pro_unpack_stick_y(const uint8_t* data) {
    return (((uint16_t) data[1] & 0xf0) >> 4) | ((uint16_t) data[2] << 4);
}

static void switch2_pro_set_input_from_packet(const uint8_t* data, uint16_t len) {
    if (len < 11 || !is_switch2_pro_ble_mode()) {
        return;
    }

    uint8_t next[64];
    memcpy(next, switch_pro_current_input, sizeof(next));
    next[0] = 0x09;
    next[2] = 0x20;  // USB connected, battery OK
    next[3] = data[2];
    next[4] = data[3];
    // BLE special byte has Capture at bit4; USB report has it at bit1
    next[5] = (data[4] & 0x0d) | ((data[4] >> 3) & 0x02);

    uint16_t lx = switch2_pro_axis12_from_state_or_report(0x00010030, switch2_pro_unpack_stick_x(data + 5));
    uint16_t ly = switch2_pro_axis12_from_state_or_report(0x00010031, switch2_pro_unpack_stick_y(data + 5));
    uint16_t rx = switch2_pro_axis12_from_state_or_report(0x00010033, switch2_pro_unpack_stick_x(data + 8));
    uint16_t ry = switch2_pro_axis12_from_state_or_report(0x00010035, switch2_pro_unpack_stick_y(data + 8));

    switch_pro_note_axis(0, lx << 4);
    switch_pro_note_axis(1, ly << 4);
    switch_pro_note_axis(2, rx << 4);
    switch_pro_note_axis(3, ry << 4);
    pack_switch2_pro_stick12(next + 6, lx, ly);
    pack_switch2_pro_stick12(next + 9, rx, ry);
    next[12] = 0x38;

    bool buttons_changed = next[3] != switch_pro_current_input[3] || next[4] != switch_pro_current_input[4] || next[5] != switch_pro_current_input[5];
    memcpy(switch_pro_current_input, next, sizeof(switch_pro_current_input));
    switch_pro_translated_reports++;
    switch_pro_input_enabled = true;

    int64_t now = k_uptime_get();
    if (buttons_changed && (now - switch_pro_last_button_log_ms >= 20)) {
        switch_pro_last_button_log_ms = now;
        switch2_flight_record(Switch2FlightEvent::BLE_INPUT, switch_pro_current_input[3], switch_pro_current_input[4], switch_pro_current_input[5], switch_pro_current_input[1], switch_pro_current_input, sizeof(switch_pro_current_input));
        LOG_INF("switch2_pro buttons=%02x %02x %02x sticks=%02x %02x %02x %02x %02x %02x out_q=%u",
            switch_pro_current_input[3], switch_pro_current_input[4], switch_pro_current_input[5],
            switch_pro_current_input[6], switch_pro_current_input[7], switch_pro_current_input[8],
            switch_pro_current_input[9], switch_pro_current_input[10], switch_pro_current_input[11],
            debug_outgoing_report_count());
    }
}

static void apply_switch_pro_hat(uint8_t hat, uint8_t* buttons2) {
    if (hat > 7) {
        return;
    }

    if (hat == 3 || hat == 4 || hat == 5) {
        *buttons2 |= BIT(0);  // Down
    }
    if (hat == 7 || hat == 0 || hat == 1) {
        *buttons2 |= BIT(1);  // Up
    }
    if (hat == 1 || hat == 2 || hat == 3) {
        *buttons2 |= BIT(2);  // Right
    }
    if (hat == 5 || hat == 6 || hat == 7) {
        *buttons2 |= BIT(3);  // Left
    }
}

static void switch_pro_translate_report(const uint8_t* report_with_id, uint8_t len) {
    if ((len < 12) || (report_with_id[0] != 0x30)) {
        return;
    }

    uint8_t next[64];
    memcpy(next, switch_pro_current_input, sizeof(next));
    next[0] = 0x30;
    next[2] = 0x91;
    next[3] = 0;
    next[4] = 0x80;
    next[5] = 0;

    if (report_button_pressed(report_with_id, 1)) next[3] |= BIT(0);   // Y
    if (report_button_pressed(report_with_id, 4)) next[3] |= BIT(1);   // X
    if (report_button_pressed(report_with_id, 2)) next[3] |= BIT(2);   // B
    if (report_button_pressed(report_with_id, 3)) next[3] |= BIT(3);   // A
    if (report_button_pressed(report_with_id, 6)) next[3] |= BIT(6);   // R
    if (report_button_pressed(report_with_id, 8)) next[3] |= BIT(7);   // ZR
    if (report_button_pressed(report_with_id, 9)) next[4] |= BIT(0);   // Minus
    if (report_button_pressed(report_with_id, 10)) next[4] |= BIT(1);  // Plus
    if (report_button_pressed(report_with_id, 12)) next[4] |= BIT(2);  // RS
    if (report_button_pressed(report_with_id, 11)) next[4] |= BIT(3);  // LS
    if (report_button_pressed(report_with_id, 13)) next[4] |= BIT(4);  // Home
    if (report_button_pressed(report_with_id, 14)) next[4] |= BIT(5);  // Capture
    if (report_button_pressed(report_with_id, 5)) next[5] |= BIT(6);   // L
    if (report_button_pressed(report_with_id, 7)) next[5] |= BIT(7);   // ZL

    apply_switch_pro_hat(report_with_id[11] & 0x0f, &next[5]);
    uint16_t lx = switch_pro_normalize_axis(
        switch_pro_axis_from_state_or_report(0x00010030, report_axis_16(report_with_id, 3)), false);
    uint16_t ly = switch_pro_normalize_axis(
        switch_pro_axis_from_state_or_report(0x00010031, report_axis_16(report_with_id, 5)), true);
    uint16_t rx = switch_pro_normalize_axis(
        switch_pro_axis_from_state_or_report(0x00010032, report_axis_16(report_with_id, 7)), false);
    uint16_t ry = switch_pro_normalize_axis(
        switch_pro_axis_from_state_or_report(0x00010035, report_axis_16(report_with_id, 9)), true);
    switch_pro_note_axis(0, lx);
    switch_pro_note_axis(1, ly);
    switch_pro_note_axis(2, rx);
    switch_pro_note_axis(3, ry);
    pack_switch_pro_stick(next + 6, lx, ly);
    pack_switch_pro_stick(next + 9, rx, ry);
    next[12] = 0x0c;

    bool buttons_changed = next[3] != switch_pro_current_input[3] || next[4] != switch_pro_current_input[4] || next[5] != switch_pro_current_input[5];
    memcpy(switch_pro_current_input, next, sizeof(switch_pro_current_input));
    switch_pro_translated_reports++;

    int64_t now = k_uptime_get();
    if (buttons_changed && (now - switch_pro_last_button_log_ms >= 20)) {
        switch_pro_last_button_log_ms = now;
        LOG_INF("switch_pro buttons=%02x %02x %02x sticks=%02x %02x %02x %02x %02x %02x out_q=%u report_q=%u",
            switch_pro_current_input[3], switch_pro_current_input[4], switch_pro_current_input[5],
            switch_pro_current_input[6], switch_pro_current_input[7], switch_pro_current_input[8],
            switch_pro_current_input[9], switch_pro_current_input[10], switch_pro_current_input[11],
            debug_outgoing_report_count(), queue_depth(&report_q));
    }
}

static void switch_pro_queue_response(const uint8_t* response, size_t len) {
    uint8_t buf[65] = {};  // byte[0]=actual length, bytes[1..64]=data
    if (len > 64) len = 64;
    buf[0] = (uint8_t)len;
    memcpy(buf + 1, response, len);
    if (is_switch2_pro_mode()) {
        switch2_flight_record(Switch2FlightEvent::QUEUE_RESPONSE, response[0], response[1], response[2], response[3], response, (uint8_t) len);
    }
    if (k_msgq_put(&switch_pro_response_q, buf, K_NO_WAIT)) {
        switch_pro_response_drops++;
    }
    note_switch_pro_response_q_depth();
}

static void switch_pro_queue_81(uint8_t command) {
    uint8_t response[64] = { 0x81, command };

    if (command == 0x01) {
        static const uint8_t mac_response[] = { 0x81, 0x01, 0x00, 0x03, 0x1f, 0x86, 0x1d, 0xd6, 0x03, 0x04 };
        memcpy(response, mac_response, sizeof(mac_response));
    }

    switch_pro_queue_response(response, sizeof(response));
}

static void switch_pro_queue_21(uint8_t subcommand, uint8_t ack, const uint8_t* data, uint8_t data_len) {
    uint8_t response[64] = {};
    response[0] = 0x21;
    response[1] = switch_pro_timer++;
    memcpy(response + 2, switch_pro_current_input + 2, 11);
    response[13] = ack;
    response[14] = subcommand;
    if (data && data_len) {
        if (data_len > sizeof(response) - 15) {
            data_len = sizeof(response) - 15;
        }
        memcpy(response + 15, data, data_len);
    }
    switch_pro_queue_response(response, sizeof(response));
}

static void switch2_pro_command_header(uint8_t* response, uint8_t cmd, uint8_t seq, uint8_t sub) {
    response[0] = cmd;
    response[1] = 0x01;
    response[2] = seq;
    response[3] = sub;
    response[4] = 0x00;
    response[5] = 0xf8;
    response[6] = 0x00;
    response[7] = 0x00;
}

static void switch2_pro_queue_command_header(uint8_t cmd, uint8_t seq, uint8_t sub) {
    uint8_t response[8] = {};
    switch2_pro_command_header(response, cmd, seq, sub);
    switch_pro_queue_response(response, sizeof(response));
}

static void switch2_pro_encode_stick_calibration(uint8_t* out) {
    pack_switch2_pro_stick12(out + 0, 0x0800, 0x0800);
    pack_switch2_pro_stick12(out + 3, 0x07ff, 0x07ff);
    pack_switch2_pro_stick12(out + 6, 0x0800, 0x0800);
}

static void switch2_pro_fill_flash_block(uint32_t address, uint8_t* block, size_t len) {
    memset(block, 0xff, len);  // erased flash default

    if (address == 0x00013080) {
        // Stick calibration page 1 — real captured bytes (byte-identical across sessions)
        static const uint8_t cal_80[64] = {
            0x01, 0xad, 0xd9, 0x9a, 0x55, 0x56, 0x65, 0xa0,
            0x00, 0x0a, 0xa0, 0x00, 0x0a, 0xe2, 0x20, 0x0e,
            0xe2, 0x20, 0x0e, 0x9a, 0xad, 0xd9, 0x9a, 0xad,
            0xd9, 0x0a, 0xa5, 0x50, 0x0a, 0xa5, 0x50, 0x2f,
            0xf6, 0x62, 0x2f, 0xf6, 0x62, 0x0a, 0xff, 0xff,
            0x82, 0xf7, 0x81, 0x56, 0x36, 0x61, 0x38, 0x86,
            0x5f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        };
        size_t n = len < sizeof(cal_80) ? len : sizeof(cal_80);
        memcpy(block, cal_80, n);
    } else if (address == 0x000130c0) {
        // Stick calibration page 2 — real captured bytes
        static const uint8_t cal_c0[64] = {
            0x01, 0xad, 0xd9, 0x9a, 0x55, 0x56, 0x65, 0xa0,
            0x00, 0x0a, 0xa0, 0x00, 0x0a, 0xe2, 0x20, 0x0e,
            0xe2, 0x20, 0x0e, 0x9a, 0xad, 0xd9, 0x9a, 0xad,
            0xd9, 0x0a, 0xa5, 0x50, 0x0a, 0xa5, 0x50, 0x2f,
            0xf6, 0x62, 0x2f, 0xf6, 0x62, 0x0a, 0xff, 0xff,
            0x33, 0x78, 0x82, 0xc4, 0x55, 0x5f, 0x0e, 0x46,
            0x62, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        };
        size_t n = len < sizeof(cal_c0) ? len : sizeof(cal_c0);
        memcpy(block, cal_c0, n);
    } else if (address == 0x00013040) {
        // Opaque 16-byte block — real captured bytes
        static const uint8_t blk_40[16] = {
            0xe8, 0xc1, 0xca, 0x41, 0xbf, 0xfe, 0xd9, 0x3a,
            0x8a, 0x67, 0x14, 0xbb, 0x5c, 0x14, 0x32, 0xbb,
        };
        size_t n = len < sizeof(blk_40) ? len : sizeof(blk_40);
        memcpy(block, blk_40, n);
    } else if (address == 0x00013100) {
        // Manufacturing string block — real captured bytes
        static const uint8_t blk_100[24] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x4d, 0x41, 0x4e, 0x3d,
            0xc6, 0x3a, 0xe4, 0x3d, 0x31, 0x61, 0x1d, 0x41,
        };
        size_t n = len < sizeof(blk_100) ? len : sizeof(blk_100);
        memcpy(block, blk_100, n);
    }
    // 0x001fc040 and 0x00013060 → all 0xff (handled by memset above)
}

static void switch2_pro_handle_flash_command(uint8_t seq, uint8_t sub, const uint8_t* report, uint8_t len) {
    if ((sub == 0x01 || sub == 0x04) && len >= 16) {
        uint8_t read_len = report[8];
        uint32_t address = (uint32_t) report[12] |
            ((uint32_t) report[13] << 8) |
            ((uint32_t) report[14] << 16) |
            ((uint32_t) report[15] << 24);

        uint8_t flash_data[64] = {};
        switch2_pro_fill_flash_block(address, flash_data, read_len <= 64 ? read_len : 64);

        uint8_t response[64] = {};
        switch2_pro_command_header(response, 0x02, seq, sub);
        response[8] = read_len;
        response[12] = report[12];
        response[13] = report[13];
        response[14] = report[14];
        response[15] = report[15];

        // First packet: header(8) + meta(8) + up to 48 bytes of data = up to 64 bytes
        uint8_t data_in_first = read_len <= 48 ? read_len : 48;
        memcpy(response + 16, flash_data, data_in_first);
        switch_pro_queue_response(response, 16 + data_in_first);

        // Second packet if data > 48 bytes (64-byte reads need a 16-byte tail packet)
        if (read_len > 48) {
            uint8_t tail[16] = {};
            memcpy(tail, flash_data + 48, read_len - 48);
            switch_pro_queue_response(tail, read_len - 48);
        }
    } else {
        uint8_t response[8] = {};
        switch2_pro_command_header(response, 0x02, seq, sub);
        switch_pro_queue_response(response, sizeof(response));
    }
}

static uint8_t switch2_pro_feature_mask = 0;
static uint8_t switch2_pro_feature_flags = 0x01 | 0x02;
static uint8_t switch2_pro_active_report_id = 0x09;

static void switch2_pro_feature_info(uint8_t flags, uint8_t* out) {
    memset(out, 0, 8);
    if (flags & 0x01) out[0] = 0x07;  // buttons
    if (flags & 0x02) out[1] = 0x07;  // sticks
    if (flags & 0x04) out[2] = 0x01;  // IMU
    if (flags & 0x10) out[4] = 0x03;  // mouse
    if (flags & 0x20) out[5] = 0x03;  // rumble
}

static void switch2_pro_handle_feature_command(uint8_t seq, uint8_t sub, const uint8_t* report, uint8_t len) {
    uint8_t response[20] = {};  // 8-byte header + up to 12 bytes data
    uint8_t flags = len >= 9 ? report[8] : 0;
    switch2_pro_command_header(response, 0x0c, seq, sub);
    uint8_t resp_len = 12;  // default: header + 4 zeroes

    switch (sub) {
        case 0x01:
            switch2_pro_feature_info(flags, response + 12);
            resp_len = 20;  // header + 4 + 8 feature bytes
            break;
        case 0x02:
            switch2_pro_feature_mask = flags;
            break;
        case 0x03:
            switch2_pro_feature_mask = 0;
            switch2_pro_feature_flags = 0;
            break;
        case 0x04:
            switch2_pro_feature_flags |= switch2_pro_feature_mask ? (flags & switch2_pro_feature_mask) : flags;
            break;
        case 0x05:
            switch2_pro_feature_flags &= ~(switch2_pro_feature_mask ? (flags & switch2_pro_feature_mask) : flags);
            break;
        default:
            break;
    }

    if (switch2_pro_feature_flags) {
        switch_pro_current_input[12] = 0x00;
    }
    switch_pro_queue_response(response, resp_len);
}

// Proactive init packets the real controller sends after responding to first 0x0d.
// Captured from real SW2 Pro (switch2-usb-re/captures/20260616-113304.log):
//   07 01 00 01 00 f8 00 00 00
//   16 01 00 01 00 f8 00 00 [24 bytes of io/mem data]
//   0b 01 00 07 00 f8 00 00
//   15 01 00 01 00 f8 00 00 01 04 01 [6-byte BT MAC]
// Without these the console stays in a 0x0d retry loop and never shows the controller.

static void switch2_pro_handle_usb_command(uint8_t seq, uint8_t sub, const uint8_t* report, uint8_t len) {
    switch (sub) {
        case 0x03: {
            switch_pro_input_enabled = len >= 9 && report[8] != 0;
            switch2_flight_record(Switch2FlightEvent::INPUT_ENABLE, sub, switch_pro_input_enabled, len >= 9 ? report[8] : 0, 0, report, len);
            uint8_t response[9] = {};
            switch2_pro_command_header(response, 0x03, seq, sub);
            response[8] = 0x01;
            switch_pro_queue_response(response, sizeof(response));
            break;
        }
        case 0x0d: {
            switch_pro_input_enabled = true;
            sw2_lr_autopress_ms = 0;  // reset auto-press for new session
            switch2_flight_record(Switch2FlightEvent::INPUT_ENABLE, sub, switch_pro_input_enabled, len >= 9 ? report[8] : 0, 0, report, len);
            // Rate-limit 0x0d acks to prevent response queue flood death spiral.
            // Console floods 0x0d when it doesn't get a timely ack; we purge
            // stale responses and send exactly one ack per 200ms window.
            static int64_t last_0d_ms = -1000;
            int64_t now = k_uptime_get();
            if (now - last_0d_ms >= 200) {
                last_0d_ms = now;
                k_msgq_purge(&switch_pro_response_q);
                uint8_t response[12] = {};
                switch2_pro_command_header(response, 0x03, seq, sub);
                response[8] = 0x01;
                switch_pro_queue_response(response, sizeof(response));
            }
            break;
        }
        case 0x0a:
            if (len >= 9 && (report[8] == 0x05 || report[8] == 0x09)) {
                switch2_pro_active_report_id = report[8];
            }
            switch2_pro_queue_command_header(0x03, seq, sub);
            break;
        default:
            switch2_pro_queue_command_header(0x03, seq, sub);
            break;
    }
}

static bool switch2_pro_write_controller_output(const uint8_t* data, uint8_t len) {
    for (uint8_t i = 0; i < CONFIG_BT_MAX_CONN; i++) {
        switch2_pro_client* client = &switch2_pro_clients[i];
        if (client->conn == NULL || client->output_value_handle == 0) {
            continue;
        }
        int err = bt_gatt_write_without_response(client->conn, client->output_value_handle, data, len, false);
        if (!err) {
            switch_pro_rumble_writes++;
            return true;
        }
        switch_pro_rumble_write_fails++;
    }
    return false;
}

static void switch2_pro_handle_rumble_report(const uint8_t* report, uint8_t len) {
    switch_pro_rumble_reports++;
    switch2_flight_record(Switch2FlightEvent::RUMBLE, report[0], report[1], 0, 0, report, len);
    switch2_pro_write_controller_output(report, len);
}

static bool switch2_pro_handle_output_report(const uint8_t* report, uint8_t len) {
    if (!is_switch2_pro_mode() || len == 0) {
        return false;
    }

    switch2_flight_record(Switch2FlightEvent::HOST_CMD, report[0], len > 1 ? report[1] : 0, len > 2 ? report[2] : 0, len > 3 ? report[3] : 0, report, len);

    // LOOP DEBUG: log every host output report (0x91 handshake) over serial so the
    // console interaction is observable on J2 while J3 stays on the Switch 2.
    LOG_INF("sw2 host_cmd len=%u: %02x %02x %02x %02x %02x %02x %02x %02x", len,
        report[0], len > 1 ? report[1] : 0, len > 2 ? report[2] : 0, len > 3 ? report[3] : 0,
        len > 4 ? report[4] : 0, len > 5 ? report[5] : 0, len > 6 ? report[6] : 0, len > 7 ? report[7] : 0);

    if (len >= 2 && report[0] == 0x02 && report[1] != 0x91) {
        switch2_pro_handle_rumble_report(report, len);
        return true;
    }

    if (len < 8 || report[1] != 0x91) {
        return false;
    }

    uint8_t cmd = report[0];
    uint8_t seq = report[2];
    uint8_t sub = report[3];

    switch (cmd) {
        case 0x02:
            switch2_pro_handle_flash_command(seq, sub, report, len);
            break;
        case 0x03:
            switch2_pro_handle_usb_command(seq, sub, report, len);
            break;
        case 0x07: {
            // Init step 1: console asks for basic ack; respond with 9-byte header
            uint8_t response[9] = {};
            switch2_pro_command_header(response, cmd, seq, sub);
            switch_pro_queue_response(response, sizeof(response));
            break;
        }
        case 0x09:
            switch2_pro_queue_command_header(0x09, seq, sub);
            break;
        case 0x0b:
            // Init step 3: 8-byte header-only ack
            switch2_pro_queue_command_header(cmd, seq, sub);
            break;
        case 0x0c:
            switch2_pro_handle_feature_command(seq, sub, report, len);
            break;
        case 0x15: {
            // BT identity exchange.
            // sub=0x01: report controller MAC so console can look it up.
            // Use real controller MAC so the console can find its BLE bond and skip crypto.
            // Fill from BLE-connected controller later.
            if (sub == 0x01) {
                // Real controller MAC (little-endian: 3c:a9:ab:69:17:3d) from GreatFET capture
                static const uint8_t sw2_test_mac[6] = { 0x3d, 0x17, 0x69, 0xab, 0xa9, 0x3c };
                uint8_t response[17] = {};
                switch2_pro_command_header(response, cmd, seq, sub);
                response[8] = 0x01;
                response[9] = 0x04;
                response[10] = 0x01;
                memcpy(response + 11, sw2_test_mac, 6);
                switch_pro_queue_response(response, sizeof(response));
                // Log console-MAC payload for relay script to forward to real controller
                printk("USB_INIT bt_mac_resp sub=0x01 mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                    sw2_test_mac[5], sw2_test_mac[4], sw2_test_mac[3],
                    sw2_test_mac[2], sw2_test_mac[1], sw2_test_mac[0]);
                // AUTH01: <full 22-byte packet hex> for relay to real controller
                printk("AUTH01:");
                for (int i = 0; i < (int)len && i < 22; i++) {
                    printk("%02x", report[i]);
                }
                printk("\n");
            } else if (sub == 0x02 || sub == 0x04) {
                // Crypto challenge-response: console sends 17B challenge, we respond 25B.
                // sub=0x04: constant identity proof (never changes for this controller).
                // sub=0x02: real challenge-response; relay to real SW2 Pro via Python script.
                static const uint8_t auth_reply_04[17] = {
                    0x01, 0x5c, 0xf6, 0xee, 0x79, 0x2c, 0xdf, 0x05,
                    0xe1, 0xba, 0x2b, 0x63, 0x25, 0xc4, 0x1a, 0x5f, 0x10,
                };
                uint8_t response[25] = {};
                switch2_pro_command_header(response, cmd, seq, sub);
                if (sub == 0x04) {
                    memcpy(response + 8, auth_reply_04, 17);
                } else {
                    // sub=0x02: log challenge for Python relay, wait up to 2s for R02: response
                    const uint8_t *chal = &report[9];  // 16 challenge bytes start at report[9]
                    printk("AUTH02:%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        chal[0],chal[1],chal[2],chal[3],chal[4],chal[5],chal[6],chal[7],
                        chal[8],chal[9],chal[10],chal[11],chal[12],chal[13],chal[14],chal[15]);
                    uint8_t relay[16] = {};
                    if (sw2_wait_relay_response(relay, 2000)) {
                        response[8] = 0x01;
                        memcpy(response + 9, relay, 16);
                        printk("AUTH02_RELAY: got response\n");
                    } else {
                        // No relay: send auth failure so the console cleanly rejects
                        // rather than entering a confused state that causes crashes.
                        response[8] = 0x00;
                        printk("AUTH02_RELAY: timeout, sending auth failure\n");
                    }
                }
                switch_pro_queue_response(response, sizeof(response));
            } else if (sub == 0x03) {
                // Auth finalize: 9-byte ack with 0x01 = success
                uint8_t response[9] = {};
                switch2_pro_command_header(response, cmd, seq, sub);
                response[8] = 0x01;
                switch_pro_queue_response(response, sizeof(response));
            } else {
                uint8_t response[9] = {};
                switch2_pro_command_header(response, cmd, seq, sub);
                response[8] = 0x01;
                switch_pro_queue_response(response, sizeof(response));
            }
            break;
        }
        case 0x11: {
            // IMU/sensor calibration: sub=0x03 needs 37 bytes, sub=0x01 needs 12 bytes
            if (sub == 0x03) {
                uint8_t response[37] = {};
                switch2_pro_command_header(response, cmd, seq, sub);
                response[8] = 0x01;  // validity marker
                response[9] = 0xc0;  // from real capture: 0x03c0 sample-rate param
                response[10] = 0x03;
                // bytes[11..12] = 0x00 0x00 (zero padding)
                // bytes[13..36] = 6 IEEE-754 float32 IMU scale factors (24 bytes)
                // Use values from real capture to match console expectations
                static const uint8_t imu_cal[24] = {
                    0xe7, 0xd0, 0x1c, 0x3b,  // ~0.00244 accel sensitivity
                    0x79, 0x22, 0xa0, 0x3a,  // ~0.00122
                    0x0a, 0xe8, 0x9c, 0x42,  // ~78.45 gyro scale
                    0x58, 0xa0, 0x0b, 0x42,  // ~34.91
                    0x0a, 0xe8, 0x9c, 0x41,  // ~19.61
                    0x58, 0xa0, 0x0b, 0x41,  // ~8.73
                };
                memcpy(response + 13, imu_cal, sizeof(imu_cal));
                switch_pro_queue_response(response, sizeof(response));
            } else if (sub == 0x01) {
                uint8_t response[12] = {};
                switch2_pro_command_header(response, cmd, seq, sub);
                response[8] = 0x03;
                switch_pro_queue_response(response, sizeof(response));
            } else {
                switch2_pro_queue_command_header(cmd, seq, sub);
            }
            break;
        }
        case 0x01: {
            // Firmware version query: sub=0x0c → 12 bytes
            uint8_t response[12] = {};
            switch2_pro_command_header(response, cmd, seq, sub);
            if (sub == 0x0c) {
                response[8] = 0x61; response[9] = 0x12; response[10] = 0x50; response[11] = 0x10;
            }
            switch_pro_queue_response(response, sizeof(response));
            break;
        }
        case 0x10: {
            // USB/HID config query: sub=0x01 → 20 bytes
            uint8_t response[20] = {};
            switch2_pro_command_header(response, cmd, seq, sub);
            if (sub == 0x01) {
                static const uint8_t d10[12] = {
                    0x02, 0x01, 0x04, 0x02, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00
                };
                memcpy(response + 8, d10, sizeof(d10));
            }
            switch_pro_queue_response(response, sizeof(response));
            break;
        }
        case 0x18: {
            // Power/audio config query: sub=0x01 → 16 bytes
            uint8_t response[16] = {};
            switch2_pro_command_header(response, cmd, seq, sub);
            if (sub == 0x01) {
                static const uint8_t d18[8] = {
                    0x00, 0x00, 0x40, 0xf0, 0x00, 0x00, 0x60, 0x00
                };
                memcpy(response + 8, d18, sizeof(d18));
            }
            switch_pro_queue_response(response, sizeof(response));
            break;
        }
        case 0x16: {
            // Init step 2: respond with 32-byte packet (memory map / address info)
            static const uint8_t p16_data[24] = {
                0x49, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x10, 0x4d, 0x00, 0x00, 0xfe, 0x4c, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            uint8_t response[32] = {};
            switch2_pro_command_header(response, cmd, seq, sub);
            memcpy(response + 8, p16_data, sizeof(p16_data));
            switch_pro_queue_response(response, sizeof(response));
            break;
        }
        default:
            switch2_pro_queue_command_header(cmd, seq, sub);
            break;
    }
    return true;
}

static void switch_pro_spi_read(const uint8_t* args, uint8_t args_len) {
    uint8_t data[48];
    memset(data, 0xff, sizeof(data));
    if (args_len < 5) {
        switch_pro_queue_21(0x10, 0x90, data, sizeof(data));
        return;
    }

    uint32_t address = (uint32_t) args[0] | ((uint32_t) args[1] << 8) | ((uint32_t) args[2] << 16) | ((uint32_t) args[3] << 24);
    uint8_t read_len = args[4];
    memcpy(data, args, 5);

    static const uint8_t stick_cal[] = {
        0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00,
        0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80
    };
    static const uint8_t body_color[] = { 0x46, 0x46, 0x46 };
    static const uint8_t button_color[] = { 0xff, 0xff, 0xff };

    if (address == 0x0000603d) {
        memcpy(data + 5, stick_cal, MIN(read_len, (uint8_t) sizeof(stick_cal)));
    } else if (address == 0x00006050) {
        memcpy(data + 5, stick_cal, MIN(read_len, (uint8_t) sizeof(stick_cal)));
    } else if (address == 0x00006086) {
        memcpy(data + 5, body_color, MIN(read_len, (uint8_t) sizeof(body_color)));
    } else if (address == 0x00006089) {
        memcpy(data + 5, button_color, MIN(read_len, (uint8_t) sizeof(button_color)));
    }

    switch_pro_queue_21(0x10, 0x90, data, MIN((uint8_t) sizeof(data), (uint8_t) (read_len + 5)));
}

// ---- Switch HD rumble stateful decoder (ported from ndeadly/MissionControl) ----
//
// The Switch sends 4-byte rumble packets per side as a packed 32-bit LE word.
// Bits [31:30] select one of four packet formats; each format encodes up to
// three amplitude+frequency samples for the LO (160 Hz) and HI (320 Hz) bands.
// Amplitude is in log2 space [-8, 0], where -8 ≈ silence and 0 = full scale.

struct SwitchRumbleState {
    float lo_amp;   // log2 amplitude LO band, [-8, 0], -8 = silence
    float lo_freq;  // log2 frequency LO band, [-2, 2], 0 = 160 Hz
    float hi_amp;
    float hi_freq;
};

// [0]=left, [1]=right
static SwitchRumbleState switch_pro_rumble_state[2] = {
    { -8.0f, 0.0f, -8.0f, 0.0f },
    { -8.0f, 0.0f, -8.0f, 0.0f },
};

static float srumble_am7(uint8_t idx) {
    if (idx == 0)   return -8.0f;
    if (idx < 16)   return 0.25f    * idx - 7.75f;
    if (idx < 32)   return 0.0625f  * idx - 4.9375f;
    return           0.03125f * idx - 3.96875f;
}

static float srumble_fm7(uint8_t idx) {
    return 0.03125f * idx - 2.0f;
}

static float srumble_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Actions: 0=ignore, 1=default, 2=substitute(offset), 3=sum+clamp
struct SRumbleCmd { uint8_t am; uint8_t fm; float am_off; float fm_off; };
static const SRumbleCmd srumble_cmd[32] = {
    { 1, 1,  0.0f,      0.0f      },  //  0 default/default
    { 2, 0,  0.0f,      0.0f      },  //  1 sub lo-amp
    { 2, 0, -0.5f,      0.0f      },  //  2
    { 2, 0, -1.0f,      0.0f      },  //  3
    { 2, 0, -1.5f,      0.0f      },  //  4
    { 2, 0, -2.0f,      0.0f      },  //  5
    { 2, 0, -2.5f,      0.0f      },  //  6
    { 2, 0, -3.0f,      0.0f      },  //  7
    { 2, 0, -3.5f,      0.0f      },  //  8
    { 2, 0, -4.0f,      0.0f      },  //  9
    { 2, 0, -4.5f,      0.0f      },  // 10
    { 2, 0, -5.0f,      0.0f      },  // 11
    { 0, 2,  0.0f,     -0.375f    },  // 12 sub hi-freq
    { 0, 2,  0.0f,     -0.1875f   },  // 13
    { 0, 2,  0.0f,      0.0f      },  // 14
    { 0, 2,  0.0f,      0.1875f   },  // 15
    { 0, 2,  0.0f,      0.375f    },  // 16
    { 3, 3,  0.125f,    0.03125f  },  // 17 sum both
    { 3, 0,  0.125f,    0.0f      },  // 18
    { 3, 3,  0.125f,   -0.03125f  },  // 19
    { 3, 3,  0.03125f,  0.03125f  },  // 20
    { 3, 0,  0.03125f,  0.0f      },  // 21
    { 3, 3,  0.03125f, -0.03125f  },  // 22
    { 0, 3,  0.0f,      0.03125f  },  // 23
    { 0, 0,  0.0f,      0.0f      },  // 24 ignore/ignore
    { 0, 3,  0.0f,     -0.03125f  },  // 25
    { 3, 3, -0.03125f,  0.03125f  },  // 26
    { 3, 0, -0.03125f,  0.0f      },  // 27
    { 3, 3, -0.03125f, -0.03125f  },  // 28
    { 3, 3, -0.125f,    0.03125f  },  // 29
    { 3, 0, -0.125f,    0.0f      },  // 30
    { 3, 3, -0.125f,   -0.03125f  },  // 31
};

static float srumble_apply(uint8_t action, float offset, float cur, float def, float lo, float hi) {
    switch (action) {
        case 2: return offset;
        case 3: return srumble_clampf(cur + offset, lo, hi);
        case 1: return def;
        default: return cur;
    }
}

static void srumble_apply_lo(uint8_t code, SwitchRumbleState* s) {
    const SRumbleCmd* c = &srumble_cmd[code & 0x1F];
    s->lo_amp  = srumble_apply(c->am, c->am_off, s->lo_amp,  -8.0f, -8.0f, 0.0f);
    s->lo_freq = srumble_apply(c->fm, c->fm_off, s->lo_freq,  0.0f, -2.0f, 2.0f);
}

static void srumble_apply_hi(uint8_t code, SwitchRumbleState* s) {
    const SRumbleCmd* c = &srumble_cmd[code & 0x1F];
    s->hi_amp  = srumble_apply(c->am, c->am_off, s->hi_amp,  -8.0f, -8.0f, 0.0f);
    s->hi_freq = srumble_apply(c->fm, c->fm_off, s->hi_freq,  0.0f, -2.0f, 2.0f);
}

// Decode 4-byte Switch rumble side packet into the per-side state, then
// return lo/hi amplitudes scaled to [0, 255] for the Xbox motors.
static void switch_pro_rumble_decode_side(const uint8_t* side,
                                          SwitchRumbleState* s,
                                          uint8_t* out_lf_amp,
                                          uint8_t* out_hf_amp) {
    uint32_t raw = (uint32_t)side[0]
                 | ((uint32_t)side[1] << 8)
                 | ((uint32_t)side[2] << 16)
                 | ((uint32_t)side[3] << 24);

    if (raw == 0) {
        *out_lf_amp = 0;
        *out_hf_amp = 0;
        return;
    }

    uint8_t ptype = (raw >> 30) & 0x03;

    switch (ptype) {
        case 0:
            // null packet — emit current state unchanged
            break;

        case 1: {
            uint32_t res5 = raw & 0x000FFFFF;  // one5bit.reserved = bits[19:0]
            uint8_t  res7 = raw & 0x03;        // one7bit.reserved = bits[1:0]
            if (res5 == 0) {
                // one5bit: bits[29:25]=amfm_lo, bits[24:20]=amfm_hi
                srumble_apply_lo((raw >> 25) & 0x1F, s);
                srumble_apply_hi((raw >> 20) & 0x1F, s);
            } else if (res7 == 0) {
                // one7bit: full absolute update
                s->lo_amp  = srumble_am7((raw >> 23) & 0x7F);
                s->lo_freq = srumble_fm7((raw >> 16) & 0x7F);
                s->hi_amp  = srumble_am7((raw >>  9) & 0x7F);
                s->hi_freq = srumble_fm7((raw >>  2) & 0x7F);
            } else {
                // three7bit: one absolute update + two 5-bit delta samples
                // bits[29:23]=xx_7bit, [22:18]=amfm_lo_1, [17:13]=amfm_hi_1,
                //      [12:8]=amfm_lo_2, [7:3]=amfm_hi_2, [2]=freq_sel, [0]=hi_sel
                uint8_t hi_sel   = (raw >> 0) & 0x01;
                uint8_t freq_sel = (raw >> 2) & 0x01;
                uint8_t hi_2     = (raw >>  3) & 0x1F;
                uint8_t lo_2     = (raw >>  8) & 0x1F;
                uint8_t hi_1     = (raw >> 13) & 0x1F;
                uint8_t lo_1     = (raw >> 18) & 0x1F;
                uint8_t val7     = (raw >> 23) & 0x7F;
                if (hi_sel) {
                    if (freq_sel) s->hi_freq = srumble_fm7(val7);
                    else          s->hi_amp  = srumble_am7(val7);
                } else {
                    if (freq_sel) s->lo_freq = srumble_fm7(val7);
                    else          s->lo_amp  = srumble_am7(val7);
                }
                srumble_apply_lo(lo_1, s);  srumble_apply_hi(hi_1, s);
                srumble_apply_lo(lo_2, s);  srumble_apply_hi(hi_2, s);
            }
            break;
        }

        case 2: {
            uint32_t res5 = raw & 0x3FF;  // two5bit.reserved = bits[9:0]
            if (res5 == 0) {
                // two5bit: bits[29:25]=lo_0, [24:20]=hi_0, [19:15]=lo_1, [14:10]=hi_1
                srumble_apply_lo((raw >> 25) & 0x1F, s);
                srumble_apply_hi((raw >> 20) & 0x1F, s);
                srumble_apply_lo((raw >> 15) & 0x1F, s);
                srumble_apply_hi((raw >> 10) & 0x1F, s);
            } else {
                // two7bit: bits[29:23]=am7, [22:18]=amfm_xx_0, [17:13]=amfm_lo_1,
                //          [12:8]=amfm_hi_1, [7:1]=fm7, [0]=hi_sel
                uint8_t hi_sel = (raw >>  0) & 0x01;
                uint8_t fm7    = (raw >>  1) & 0x7F;
                uint8_t hi_1   = (raw >>  8) & 0x1F;
                uint8_t lo_1   = (raw >> 13) & 0x1F;
                uint8_t xx_0   = (raw >> 18) & 0x1F;
                uint8_t am7    = (raw >> 23) & 0x7F;
                if (hi_sel) {
                    s->hi_amp  = srumble_am7(am7);
                    s->hi_freq = srumble_fm7(fm7);
                    srumble_apply_lo(xx_0, s);
                } else {
                    s->lo_amp  = srumble_am7(am7);
                    s->lo_freq = srumble_fm7(fm7);
                    srumble_apply_hi(xx_0, s);
                }
                srumble_apply_lo(lo_1, s);  srumble_apply_hi(hi_1, s);
            }
            break;
        }

        case 3:
            // three5bit: bits[29:25]=lo_0, [24:20]=hi_0, [19:15]=lo_1,
            //            [14:10]=hi_1, [9:5]=lo_2, [4:0]=hi_2
            srumble_apply_lo((raw >> 25) & 0x1F, s);
            srumble_apply_hi((raw >> 20) & 0x1F, s);
            srumble_apply_lo((raw >> 15) & 0x1F, s);
            srumble_apply_hi((raw >> 10) & 0x1F, s);
            srumble_apply_lo((raw >>  5) & 0x1F, s);
            srumble_apply_hi((raw >>  0) & 0x1F, s);
            break;
    }

    // Convert log2 amplitude to linear [0, 1], then to [0, 255].
    // Threshold at -7.9375 (matches MissionControl's AmplitudeThreshold).
    // exp2f gives linear [0,1]; apply x^1.5 to spread weak/strong apart and
    // bring the overall level down so Xbox ERM motors don't feel uniformly heavy.
    // x^1.5 at: 6%->1%, 25%->12%, 50%->35%, 100%->100%
    float lo_lin = (s->lo_amp >= -7.9375f) ? exp2f(s->lo_amp) : 0.0f;
    float hi_lin = (s->hi_amp >= -7.9375f) ? exp2f(s->hi_amp) : 0.0f;
    *out_lf_amp = (uint8_t)(lo_lin * sqrtf(lo_lin) * 255.0f);
    *out_hf_amp = (uint8_t)(hi_lin * sqrtf(hi_lin) * 255.0f);
}

static void switch_pro_rumble_write_cb(struct bt_hogp* hogp, struct bt_hogp_rep_info* rep, uint8_t err) {
    if (err) {
        switch_pro_rumble_write_fails++;
        LOG_WRN("switch_pro rumble write callback err=%u", err);
    }
}

static struct bt_hogp_rep_info* switch_pro_find_xbox_rumble_report(struct bt_hogp* hogp) {
    struct bt_hogp_rep_info* rep = bt_hogp_rep_find(hogp, BT_HIDS_REPORT_TYPE_OUTPUT, 0x03);
    if (rep != NULL) {
        return rep;
    }

    struct bt_hogp_rep_info* only_output = NULL;
    rep = NULL;
    while (NULL != (rep = bt_hogp_rep_next(hogp, rep))) {
        if (bt_hogp_rep_type(rep) != BT_HIDS_REPORT_TYPE_OUTPUT) {
            continue;
        }
        if (only_output != NULL) {
            return NULL;
        }
        only_output = rep;
    }
    return only_output;
}

static void switch_pro_send_xbox_rumble(const uint8_t* switch_rumble) {
    uint8_t left_lf, left_hf, right_lf, right_hf;
    switch_pro_rumble_decode_side(switch_rumble,     &switch_pro_rumble_state[0], &left_lf,  &left_hf);
    switch_pro_rumble_decode_side(switch_rumble + 4, &switch_pro_rumble_state[1], &right_lf, &right_hf);

    // Xbox Series X: strong motor = low-frequency, weak motor = high-frequency.
    // Take the max across left and right for each band.
    uint8_t strong = left_lf > right_lf ? left_lf : right_lf;
    uint8_t weak   = left_hf > right_hf ? left_hf : right_hf;
    bool active = strong || weak;
    uint8_t payload[8] = {
        0x0f,  // enable all four Xbox rumble motors
        0x00,
        0x00,
        strong,
        weak,
        0xff,
        0x00,
        0x01,
    };
    int64_t now = k_uptime_get();

    switch_pro_rumble_reports++;
    if (!memcmp(payload, switch_pro_last_xbox_rumble, sizeof(payload)) &&
            (now - switch_pro_last_xbox_rumble_ms < (active ? 120 : 500))) {
        return;
    }

    bool found = false;
    for (uint8_t i = 0; i < CONFIG_BT_MAX_CONN; i++) {
        if (!bt_hogp_ready_check(&hogps[i])) {
            continue;
        }

        struct bt_hogp_rep_info* rep = switch_pro_find_xbox_rumble_report(&hogps[i]);
        if (rep == NULL) {
            continue;
        }
        found = true;

        int err = bt_hogp_rep_write_wo_rsp(&hogps[i], rep, payload, sizeof(payload), switch_pro_rumble_write_cb);
        if (!err) {
            memcpy(switch_pro_last_xbox_rumble, payload, sizeof(payload));
            switch_pro_last_xbox_rumble_ms = now;
            switch_pro_rumble_writes++;
            return;
        }
        if (err == -EBUSY) {
            switch_pro_rumble_busy++;
        } else {
            switch_pro_rumble_write_fails++;
        }
    }

    if (!found) {
        switch_pro_rumble_write_fails++;
    }
}

// Send a rumble payload directly to a specific hogp (bypasses the dedup cache).
static void switch_pro_rumble_to_hogp(struct bt_hogp* hogp, uint8_t strong, uint8_t weak) {
    struct bt_hogp_rep_info* rep = switch_pro_find_xbox_rumble_report(hogp);
    if (rep == NULL) {
        return;
    }
    uint8_t payload[8] = { 0x0f, 0x00, 0x00, strong, weak, 0xff, 0x00, 0x01 };
    bt_hogp_rep_write_wo_rsp(hogp, rep, payload, sizeof(payload), switch_pro_rumble_write_cb);
}

// Stop-rumble work: sent ~250 ms after the connect buzz to silence all connected controllers.
static void connect_rumble_stop_fn(struct k_work* work) {
    for (uint8_t i = 0; i < CONFIG_BT_MAX_CONN; i++) {
        if (bt_hogp_ready_check(&hogps[i])) {
            switch_pro_rumble_to_hogp(&hogps[i], 0, 0);
        }
    }
}
static K_WORK_DELAYABLE_DEFINE(connect_rumble_stop_work, connect_rumble_stop_fn);

static void switch_pro_handle_rumble_report(const uint8_t* report, uint8_t len, bool has_report_id) {
    uint8_t base = has_report_id ? 1 : 0;
    if (len < base + 9) {
        return;
    }

    switch_pro_send_xbox_rumble(report + base + 1);
}

static void switch_pro_handle_subcommand(const uint8_t* report, uint8_t len, bool has_report_id) {
    uint8_t base = has_report_id ? 1 : 0;
    if (len <= base + 9) {
        return;
    }

    uint8_t subcommand = report[base + 9];
    const uint8_t* args = report + base + 10;
    uint8_t args_len = len - base - 10;

    switch (subcommand) {
        case 0x01: {
            static const uint8_t pairing_response[] = { 0x03 };
            switch_pro_queue_21(subcommand, 0x81, pairing_response, sizeof(pairing_response));
            break;
        }
        case 0x02: {
            static const uint8_t device_info[] = { 0x03, 0x48, 0x03, 0x02, 0x53, 0x50, 0x00, 0x01, 0x03, 0x04, 0x01, 0x01 };
            switch_pro_queue_21(subcommand, 0x82, device_info, sizeof(device_info));
            break;
        }
        case 0x04:
            switch_pro_queue_21(subcommand, 0x83, NULL, 0);
            break;
        case 0x10:
            switch_pro_spi_read(args, args_len);
            break;
        case 0x21: {
            static const uint8_t imu_status[] = { 0x01, 0x00, 0xff, 0x00, 0x03, 0x00, 0x05, 0x01 };
            switch_pro_queue_21(subcommand, 0xa0, imu_status, sizeof(imu_status));
            break;
        }
        case 0x30:
            switch_pro_input_enabled = args_len == 0 || args[0] != 0;
            switch_pro_last_input_ms = 0;
            switch_pro_queue_21(subcommand, 0x80, NULL, 0);
            break;
        default:
            switch_pro_queue_21(subcommand, 0x80, NULL, 0);
            break;
    }
}

static bool switch_pro_handle_output_report(const uint8_t* report, uint8_t len) {
    if (!is_switch_pro_mode() || len == 0) {
        return false;
    }

    // LOOP DEBUG: log every host output report at the common entry (mode 6 or 7)
    // so we can confirm the console is sending a handshake at all.
    LOG_INF("pro out_report desc=%u len=%u: %02x %02x %02x %02x", our_descriptor_number, len,
        report[0], len > 1 ? report[1] : 0, len > 2 ? report[2] : 0, len > 3 ? report[3] : 0);

    if (is_switch2_pro_mode()) {
        return switch2_pro_handle_output_report(report, len);
    }

    uint8_t report_id = report[0];
    bool has_report_id = true;

    if (report_id != 0x01 && report_id != 0x10 && report_id != 0x80 && report_id != 0x82) {
        has_report_id = false;
        report_id = len == 1 ? 0x80 : report[0];
    }

    if (report_id == 0x80) {
        uint8_t command = has_report_id ? (len > 1 ? report[1] : 0) : report[0];
        switch (command) {
            case 0x04:
                switch_pro_input_enabled = true;
                switch_pro_last_input_ms = 0;
                break;
            case 0x05:
                switch_pro_input_enabled = false;
                break;
            case 0x01:
            case 0x02:
            case 0x03:
            default:
                switch_pro_queue_81(command);
                break;
        }
        return true;
    }

    if (report_id == 0x01) {
        switch_pro_handle_rumble_report(report, len, has_report_id);
        switch_pro_handle_subcommand(report, len, has_report_id);
        return true;
    }

    if (report_id == 0x10) {
        switch_pro_handle_rumble_report(report, len, has_report_id);
        return true;
    }

    return true;
}

static bool switch_pro_send_response() {
    uint8_t slot[65];  // byte[0]=len, bytes[1..64]=data
    if (!is_switch_pro_mode() || k_msgq_get(&switch_pro_response_q, slot, K_NO_WAIT)) {
        return false;
    }
    uint8_t len = slot[0];
    uint8_t* response = slot + 1;
    bool sent;
#if CONFIG_USB_HID_DEVICE_COUNT == 1
    if (is_switch2_pro_mode()) {
        sent = CHK(usb_write(SWITCH2_VENDOR_IN_EP, response, len, NULL));
    } else
#endif
    {
        sent = CHK(hid_int_ep_write(hid_dev0, response, 64, NULL));
    }
    if (is_switch2_pro_mode()) {
        switch2_flight_record(Switch2FlightEvent::SEND_RESPONSE, response[0], sent, response[2], response[3], response, len);
    }
    if (sent) {
        switch_pro_response_writes++;
    } else {
        switch_pro_response_write_fails++;
    }
    return sent;
}

static bool switch_pro_send_input_heartbeat() {
    // Standalone emulation: in Switch 2 Pro mode, send idle input from the start
    // (no controller / no console "enable" needed) so the console notices the
    // controller and starts the 0x91 handshake.
    if (!is_switch_pro_mode() || (!switch_pro_input_enabled && !is_switch2_pro_mode())) {
        return false;
    }

    int64_t now = k_uptime_get();
    if (switch_pro_last_input_ms && (now - switch_pro_last_input_ms < 8)) {
        return false;
    }

    // When USB host hasn't started the session (no 0x0d yet), rate-limit to 4 Hz
    // to avoid flooding the USBD event queue with EAGAIN errors.
    if (is_switch2_pro_mode() && !switch_pro_input_enabled) {
        static int64_t last_sw2_probe_ms = 0;
        if (now - last_sw2_probe_ms < 250) return false;
        last_sw2_probe_ms = now;
    }

    switch_pro_current_input[1] = switch_pro_timer++;
    switch_pro_last_input_ms = now;

    // Auto-press L+R 3x after input enabled so DK self-registers on Change Grip screen
    // without requiring user touchscreen interaction (which causes console kernel panic).
    // SW2 Pro layout: byte[3] bit4=R, byte[4] bit4=L
    if (is_switch2_pro_mode() && switch_pro_input_enabled) {
        if (!sw2_lr_autopress_ms) sw2_lr_autopress_ms = now + 4000;
        int64_t since = now - sw2_lr_autopress_ms;
        if (since >= 0 && since < 1500) {
            if ((since % 500) < 150) {
                switch_pro_current_input[3] |= 0x10;  // R pressed
                switch_pro_current_input[4] |= 0x10;  // L pressed
            } else {
                switch_pro_current_input[3] &= ~0x10;
                switch_pro_current_input[4] &= ~0x10;
            }
        } else {
            switch_pro_current_input[3] &= ~0x10;
            switch_pro_current_input[4] &= ~0x10;
        }
    }

    bool sent = CHK(hid_int_ep_write(hid_dev0, switch_pro_current_input, sizeof(switch_pro_current_input), NULL));
    if (is_switch2_pro_mode()) {
        switch2_flight_record_send_input(sent);
    }
    if (sent) {
        switch_pro_heartbeat_writes++;
    } else {
        switch_pro_heartbeat_write_fails++;
    }
    // LOOP DEBUG: rate-limited (1/s) proof the board is sending input + whether
    // the console is accepting IN writes.
    static int64_t last_hb_log_ms = 0;
    if (now - last_hb_log_ms >= 1000) {
        last_hb_log_ms = now;
        LOG_INF("sw2 heartbeat: writes=%u fails=%u enabled=%u rep0=%02x rep2=%02x b34=%02x%02x autopress=%lld",
            switch_pro_heartbeat_writes, switch_pro_heartbeat_write_fails,
            switch_pro_input_enabled, switch_pro_current_input[0], switch_pro_current_input[2],
            switch_pro_current_input[3], switch_pro_current_input[4],
            (long long)(sw2_lr_autopress_ms ? now - sw2_lr_autopress_ms : -9999));
    }
    return sent;
}

static void switch_pro_fill_diagnostics(uint32_t page, uint32_t values[SWITCH_PRO_DIAG_VALUES]) {
    memset(values, 0, SWITCH_PRO_DIAG_VALUES * sizeof(uint32_t));

    switch (page) {
        case 0:
            values[0] = 0x44315053;  // SP1D
            values[1] = 1;
            values[2] = switch_pro_input_enabled;
            values[3] = (queue_depth(&report_q) & 0xffff) | (switch_pro_report_q_highwater << 16);
            values[4] = (queue_depth(&switch_pro_response_q) & 0xffff) | (switch_pro_response_q_highwater << 16);
            values[5] = debug_outgoing_report_count();
            values[6] = debug_outgoing_report_overflows();
            break;
        case 1:
            values[0] = switch_pro_ble_reports;
            values[1] = switch_pro_ble_report_drops;
            values[2] = switch_pro_host_reports;
            values[3] = switch_pro_host_report_drops;
            values[4] = switch_pro_set_reports;
            values[5] = switch_pro_translated_reports;
            values[6] = switch_pro_response_drops;
            break;
        case 2:
            values[0] = switch_pro_mapped_writes;
            values[1] = switch_pro_mapped_write_fails;
            values[2] = switch_pro_heartbeat_writes;
            values[3] = switch_pro_heartbeat_write_fails;
            values[4] = switch_pro_response_writes;
            values[5] = switch_pro_response_write_fails;
            values[6] = switch_pro_timer;
            break;
        case 3:
            values[0] = switch_pro_current_input[3] | (switch_pro_current_input[4] << 8) | (switch_pro_current_input[5] << 16);
            values[1] = switch_pro_current_input[6] | (switch_pro_current_input[7] << 8) | (switch_pro_current_input[8] << 16);
            values[2] = switch_pro_current_input[9] | (switch_pro_current_input[10] << 8) | (switch_pro_current_input[11] << 16);
            values[3] = switch_pro_current_input[1];
            values[4] = switch_pro_last_input_ms;
            values[5] = (switch_pro_bt_connected_events & 0xffff) | ((switch_pro_hogp_ready_events & 0xffff) << 16);
            values[6] = (switch_pro_bt_disconnected_events & 0xffff) | ((switch_pro_last_disconnect_reason & 0xff) << 16) | ((switch_pro_conn_count_highwater & 0xff) << 24);
            break;
        case 4:
            values[0] = switch_pro_axis_last[0] | (switch_pro_axis_last[1] << 16);
            values[1] = switch_pro_axis_last[2] | (switch_pro_axis_last[3] << 16);
            values[2] = switch_pro_axis_min[0] | (switch_pro_axis_max[0] << 16);
            values[3] = switch_pro_axis_min[1] | (switch_pro_axis_max[1] << 16);
            values[4] = switch_pro_axis_min[2] | (switch_pro_axis_max[2] << 16);
            values[5] = switch_pro_axis_min[3] | (switch_pro_axis_max[3] << 16);
            values[6] = (switch_pro_rumble_reports & 0xffff) | ((switch_pro_rumble_writes & 0xffff) << 16);
            break;
        default:
            break;
    }
}

static void switch_pro_persist_diagnostics() {
    if (!is_switch_pro_mode() || !switch_pro_input_enabled) {
        return;
    }
    if (switch_pro_ble_reports == 0 && switch_pro_translated_reports <= 2) {
        return;
    }

    int64_t now = k_uptime_get();
    if (switch_pro_last_diag_persist_ms && (now - switch_pro_last_diag_persist_ms < 2000)) {
        return;
    }
    switch_pro_last_diag_persist_ms = now;

    for (uint32_t page = 0; page < SWITCH_PRO_DIAG_PAGES; page++) {
        switch_pro_fill_diagnostics(page, switch_pro_saved_diag[page]);
    }
    settings_save_one("remapper/switch_pro_diag", switch_pro_saved_diag, sizeof(switch_pro_saved_diag));
}

static void switch_pro_log_stats() {
    if (!is_switch_pro_mode()) {
        return;
    }

    int64_t now = k_uptime_get();
    if (switch_pro_last_stats_ms && (now - switch_pro_last_stats_ms < 1000)) {
        return;
    }
    switch_pro_last_stats_ms = now;

    LOG_INF("switch_pro_stats enabled=%u report_q=%u/%u response_q=%u/%u out_q=%u out_overflows=%u ble=%u ble_drop=%u host=%u host_drop=%u set=%u translated=%u mapped=%u/%u heartbeat=%u/%u response=%u/%u response_drop=%u rumble=%u/%u fail=%u busy=%u buttons=%02x %02x %02x",
        switch_pro_input_enabled,
        queue_depth(&report_q), switch_pro_report_q_highwater,
        queue_depth(&switch_pro_response_q), switch_pro_response_q_highwater,
        debug_outgoing_report_count(), debug_outgoing_report_overflows(),
        switch_pro_ble_reports, switch_pro_ble_report_drops,
        switch_pro_host_reports, switch_pro_host_report_drops,
        switch_pro_set_reports,
        switch_pro_translated_reports,
        switch_pro_mapped_writes, switch_pro_mapped_write_fails,
        switch_pro_heartbeat_writes, switch_pro_heartbeat_write_fails,
        switch_pro_response_writes, switch_pro_response_write_fails,
        switch_pro_response_drops,
        switch_pro_rumble_reports, switch_pro_rumble_writes,
        switch_pro_rumble_write_fails, switch_pro_rumble_busy,
        switch_pro_current_input[3], switch_pro_current_input[4], switch_pro_current_input[5]);

    switch_pro_persist_diagnostics();
}

static void activity_led_off_work_fn(struct k_work* work) {
    gpio_pin_set_dt(&led0, false);
}
static K_WORK_DELAYABLE_DEFINE(activity_led_off_work, activity_led_off_work_fn);

enum class LedMode {
    OFF = 0,
    ON = 1,
    BLINK = 2,
};

static atomic_t led_blink_count = ATOMIC_INIT(0);
static atomic_t led_mode = (atomic_t) ATOMIC_INIT(LedMode::OFF);
static int led_blinks_left = 0;
static bool next_blink_state = true;

static void led_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(led_work, led_work_fn);

static void led_work_fn(struct k_work* work) {
    LedMode my_led_mode = (LedMode) atomic_get(&led_mode);
    switch (my_led_mode) {
        case LedMode::OFF:
            gpio_pin_set_dt(&led1, false);
            break;
        case LedMode::ON:
            gpio_pin_set_dt(&led1, true);
            break;
        case LedMode::BLINK: {
            int next_work = 0;
            if (next_blink_state) {
                if (led_blinks_left > 0) {
                    led_blinks_left--;
                    gpio_pin_set_dt(&led1, true);
                    next_blink_state = false;
                    next_work = 100;
                } else {
                    led_blinks_left = atomic_get(&led_blink_count);
                    gpio_pin_set_dt(&led1, false);
                    next_work = 1000;
                }
            } else {
                gpio_pin_set_dt(&led1, false);
                next_blink_state = true;
                next_work = 100;
            }
            k_work_reschedule(&led_work, K_MSEC(next_work));
            break;
        }
    }
}

static void set_led_mode(LedMode led_mode_) {
    if (atomic_set(&led_mode, (atomic_val_t) led_mode_) != (atomic_val_t) led_mode_) {
        k_work_reschedule(&led_work, K_NO_WAIT);
    }
}

static void scan_start() {
    if (CHK(bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE))) {
        LOG_INF("Scanning started.");
        scanning = true;
    }
}

static void scan_stop() {
    if (CHK(bt_scan_stop())) {
        LOG_INF("Scanning stopped.");
        scanning = false;
        set_led_mode(LedMode::BLINK);
    }
}

struct switch2_pro_adv_parse_ctx {
    bool matched;
    uint8_t sample[8];
    uint8_t sample_len;
};

static constexpr uint16_t SWITCH2_PRO_BT_COMPANY_ID = 0x0553;
static constexpr uint16_t SWITCH2_PRO_NINTENDO_USB_VID = 0x057e;
static constexpr uint16_t SWITCH2_PRO_USB_PID = 0x2069;

static bool switch2_pro_adv_parse_cb(struct bt_data* data, void* user_data) {
    switch2_pro_adv_parse_ctx* ctx = (switch2_pro_adv_parse_ctx*) user_data;
    if (data->type != BT_DATA_MANUFACTURER_DATA || data->data_len < 9) {
        return true;
    }

    const uint8_t* mfg = data->data;
    uint16_t company_id = sys_get_le16(mfg);
    if (company_id != SWITCH2_PRO_BT_COMPANY_ID) {
        return true;
    }

    ctx->sample_len = MIN(data->data_len, (uint8_t) sizeof(ctx->sample));
    memcpy(ctx->sample, mfg, ctx->sample_len);

    uint16_t vid = sys_get_le16(mfg + 5);
    uint16_t pid = sys_get_le16(mfg + 7);
    if (vid == SWITCH2_PRO_NINTENDO_USB_VID && pid == SWITCH2_PRO_USB_PID) {
        ctx->matched = true;
        return false;
    }
    return true;
}

static bool switch2_pro_adv_matches(struct net_buf_simple* adv_data) {
    switch2_pro_adv_parse_ctx ctx = {};
    bt_data_parse(adv_data, switch2_pro_adv_parse_cb, &ctx);
    return ctx.matched;
}

static void process_bond(const struct bt_bond_info* info, void* user_data) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(&info->addr, addr_str, sizeof(addr_str));
    LOG_DBG("%s", addr_str);
    (*((int*) user_data))++;
    CHK(bt_scan_filter_add(BT_SCAN_FILTER_TYPE_ADDR, &info->addr));
}

static void count_conn_cb(struct bt_conn* conn, void* data) {
    (*((int*) data))++;
}

static int count_connections() {
    int conn_count = 0;
    bt_conn_foreach(BT_CONN_TYPE_LE, count_conn_cb, &conn_count);
    atomic_set(&led_blink_count, conn_count);
    return conn_count;
}

static bool scan_setup_filters() {
    bt_scan_filter_remove_all();

    if (is_switch2_pro_ble_mode()) {
        LOG_INF("scanning for Switch 2 Pro Controller manufacturer data");
        int bonded_count = 0;
        bt_foreach_bond(BT_ID_DEFAULT, process_bond, &bonded_count);
        switch2_flight_record(Switch2FlightEvent::SCAN, bonded_count, peers_only, 0, 0);
        static uint8_t nintendo_mfg_prefix[] = { 0x53, 0x05 };
        struct bt_scan_manufacturer_data nintendo_mfg = {
            .data = nintendo_mfg_prefix,
            .data_len = sizeof(nintendo_mfg_prefix),
        };
        if (!CHK(bt_scan_filter_add(BT_SCAN_FILTER_TYPE_MANUFACTURER_DATA, &nintendo_mfg))) {
            return false;
        }
        if (!CHK(bt_scan_filter_enable(BT_SCAN_MANUFACTURER_DATA_FILTER, false))) {
            return false;
        }
        peers_only = false;
        return true;
    }

    if (!CHK(bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, (struct bt_uuid*) &BT_UUID_HIDS_))) {
        return false;
    }

    int bonded_count = 0;
    bt_foreach_bond(BT_ID_DEFAULT, process_bond, &bonded_count);

    int conn_count = count_connections();

    uint8_t filter_mode = BT_SCAN_UUID_FILTER;

    if (peers_only && (bonded_count > 0)) {
        if (conn_count == bonded_count) {
            LOG_DBG("all bonded peers connected, not scanning");
            return false;
        }
        filter_mode |= BT_SCAN_ADDR_FILTER;
        LOG_DBG("scanning for bonded peers only");
    } else {
        LOG_DBG("scanning for new peers");
        peers_only = false;
    }

    if (!CHK(bt_scan_filter_enable(filter_mode, true))) {
        return false;
    }

    return true;
}

static void scan_start_work_fn(struct k_work* work) {
    if (scanning) {
        scan_stop();
    }
    if (scan_setup_filters()) {
        scan_start();
        set_led_mode(peers_only ? LedMode::BLINK : LedMode::ON);
    } else {
        set_led_mode(LedMode::BLINK);
    }
}
static K_WORK_DELAYABLE_DEFINE(scan_start_work, scan_start_work_fn);

static void scan_stop_work_fn(struct k_work* work) {
    scan_stop();
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}
static K_WORK_DEFINE(scan_stop_work, scan_stop_work_fn);

static void disconnect_conn(struct bt_conn* conn, void* data) {
    CHK(bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN));
}

static void clear_bonds_work_fn(struct k_work* work) {
    if (CHK(bt_unpair(BT_ID_DEFAULT, &BT_ADDR_LE_ANY_))) {
        LOG_INF("Bonds cleared.");
    } else {
        return;
    }

    scan_stop();
    bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_conn, NULL);
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}
static K_WORK_DEFINE(clear_bonds_work, clear_bonds_work_fn);

static void scan_filter_match(struct bt_scan_device_info* device_info, struct bt_scan_filter_match* filter_match, bool connectable) {
    struct bt_conn* conn;
    char addr[BT_ADDR_LE_STR_LEN];

    if (is_switch2_pro_ble_mode()) {
        bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
        if (!connectable || !switch2_pro_adv_matches(device_info->adv_data)) {
            LOG_INF("ignoring Nintendo manufacturer data from %s connectable=%u", addr, connectable);
            return;
        }
        switch2_pro_adv_parse_ctx ctx = {};
        bt_data_parse(device_info->adv_data, switch2_pro_adv_parse_cb, &ctx);
        LOG_INF("Switch 2 Pro filter match: %s adv_type=%u mfg=%02x %02x %02x %02x %02x %02x %02x %02x",
            addr, device_info->recv_info->adv_type,
            ctx.sample[0], ctx.sample[1], ctx.sample[2], ctx.sample[3],
            ctx.sample[4], ctx.sample[5], ctx.sample[6], ctx.sample[7]);
        switch2_flight_record(Switch2FlightEvent::SCAN, 1, connectable, device_info->recv_info->adv_type, ctx.sample_len, ctx.sample, ctx.sample_len);
        scan_stop();
        bt_addr_le_copy(&pending_switch2_pro_addr, device_info->recv_info->addr);
        pending_switch2_pro_valid = true;
        if (CHK(bt_conn_le_create(device_info->recv_info->addr, &BT_CONN_LE_CREATE_CONN_, conn_param, &conn))) {
            bt_conn_unref(conn);
        } else {
            pending_switch2_pro_valid = false;
            k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
        }
        return;
    }

    if (!filter_match->uuid.match || (filter_match->uuid.count != 1)) {
        LOG_WRN("%s invalid device connected", __func__);
        return;
    }

    bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

    LOG_INF("%s address: %s connectable: %s", __func__, addr, connectable ? "yes" : "no");
}

static void scan_connecting_error(struct bt_scan_device_info* device_info) {
    LOG_WRN("");
}

static void scan_connecting(struct bt_scan_device_info* device_info, struct bt_conn* conn) {
    LOG_INF("");
}

// XXX this hasn't been tested in practice
static void scan_filter_no_match(struct bt_scan_device_info* device_info, bool connectable) {
    struct bt_conn* conn;
    char addr[BT_ADDR_LE_STR_LEN];

    if (is_switch2_pro_ble_mode() && connectable && switch2_pro_adv_matches(device_info->adv_data)) {
        bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
        switch2_pro_adv_parse_ctx ctx = {};
        bt_data_parse(device_info->adv_data, switch2_pro_adv_parse_cb, &ctx);
        LOG_INF("Switch 2 Pro advertisement from %s adv_type=%u mfg=%02x %02x %02x %02x %02x %02x %02x %02x",
            addr, device_info->recv_info->adv_type,
            ctx.sample[0], ctx.sample[1], ctx.sample[2], ctx.sample[3],
            ctx.sample[4], ctx.sample[5], ctx.sample[6], ctx.sample[7]);
        switch2_flight_record(Switch2FlightEvent::SCAN, 2, connectable, device_info->recv_info->adv_type, ctx.sample_len, ctx.sample, ctx.sample_len);
        scan_stop();
        bt_addr_le_copy(&pending_switch2_pro_addr, device_info->recv_info->addr);
        pending_switch2_pro_valid = true;

        if (CHK(bt_conn_le_create(device_info->recv_info->addr, &BT_CONN_LE_CREATE_CONN_, conn_param, &conn))) {
            bt_conn_unref(conn);
        } else {
            pending_switch2_pro_valid = false;
            k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
        }
        return;
    }

    if (device_info->recv_info->adv_type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
        LOG_INF("Direct advertising received from %s", addr);
        scan_stop();  // XXX

        if (CHK(bt_conn_le_create(device_info->recv_info->addr, &BT_CONN_LE_CREATE_CONN_, device_info->conn_param, &conn))) {
            bt_conn_unref(conn);
        }
    }
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_filter_no_match, scan_connecting_error, scan_connecting);

// This is a workaround for the Xbox Adaptive Controller that sends UUIDs like this:
// 00002a4a-0000-0000-0000-000000000000 in Find Information responses.
// This is not the correct UUID128 representation for UUID16(2a4a), which would be:
// 00002a4a-0000-1000-8000-00805f9b34fb
static void patch_broken_uuids(struct bt_gatt_dm* dm) {
    const struct bt_gatt_dm_attr* attr = NULL;
    char str1[BT_UUID_STR_LEN];
    char str2[BT_UUID_STR_LEN];

    while (NULL != (attr = bt_gatt_dm_attr_next(dm, attr))) {
        if (attr->uuid->type == BT_UUID_TYPE_128) {
            bool needs_fix = true;
            for (int i = 0; i < 16; i++) {
                if ((i != 12) && (i != 13) && (BT_UUID_128(attr->uuid)->val[i] != 0)) {
                    needs_fix = false;
                    break;
                }
            }
            if (needs_fix) {
                bt_uuid_to_str(attr->uuid, str1, sizeof(str2));
                *((bt_uuid_16*) attr->uuid) = {
                    .uuid = { BT_UUID_TYPE_16 },
                    .val = (BT_UUID_128(attr->uuid)->val[13] << 8 | BT_UUID_128(attr->uuid)->val[12])
                };
                bt_uuid_to_str(attr->uuid, str2, sizeof(str2));
                LOG_INF("%s -> %s", str1, str2);
            }
        }
    }
}

static void discovery_completed_cb(struct bt_gatt_dm* dm, void* context) {
    LOG_INF("");
    patch_broken_uuids(dm);
    CHK(bt_hogp_handles_assign(dm, ((struct bt_hogp*) context)));  // XXX disconnect if this fails?
    CHK(bt_gatt_dm_data_release(dm));
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}

static void discovery_service_not_found_cb(struct bt_conn* conn, void* context) {
    LOG_WRN("");
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}

static void discovery_error_found_cb(struct bt_conn* conn, int err, void* context) {
    LOG_ERR("err=%d", err);
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}

static const struct bt_gatt_dm_cb discovery_cb = {
    .completed = discovery_completed_cb,
    .service_not_found = discovery_service_not_found_cb,
    .error_found = discovery_error_found_cb,
};

static void gatt_discover(struct bt_conn* conn) {
    uint8_t conn_idx = bt_conn_index(conn);
    if (!CHK(bt_gatt_dm_start(conn, (struct bt_uuid*) &BT_UUID_HIDS_, &discovery_cb, &hogps[conn_idx]))) {
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
    }
}

static constexpr uint16_t SWITCH2_PRO_INPUT_VALUE_HANDLE = 0x000a;
static constexpr uint16_t SWITCH2_PRO_INPUT_CCC_HANDLE = 0x000b;
static constexpr uint16_t SWITCH2_PRO_OUTPUT_VALUE_HANDLE = 0x0012;
static constexpr uint16_t SWITCH2_PRO_CMD_VALUE_HANDLE = 0x0014;
static constexpr uint16_t SWITCH2_PRO_ACK_VALUE_HANDLE = 0x001a;
static constexpr uint16_t SWITCH2_PRO_ACK_CCC_HANDLE = 0x001b;
static constexpr int64_t SWITCH2_PRO_INIT_ACK_TIMEOUT_MS = 8000;

static void switch2_pro_send_init_cmd(switch2_pro_client* client);

static void switch2_pro_init_retry_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(switch2_pro_init_retry_work, switch2_pro_init_retry_work_fn);

static uint8_t switch2_pro_notify_cb(struct bt_conn* conn, struct bt_gatt_subscribe_params* params, const void* data, uint16_t length) {
    if (data == NULL) {
        return BT_GATT_ITER_STOP;
    }

    k_work_reschedule(&activity_led_off_work, K_MSEC(50));
    gpio_pin_set_dt(&led0, true);

    if (scanning) {
        scanning = false;
        k_work_submit(&scan_stop_work);
    } else {
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
    }

    switch_pro_ble_reports++;
    switch2_pro_set_input_from_packet((const uint8_t*) data, length);
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t switch2_pro_ack_notify_cb(struct bt_conn* conn, struct bt_gatt_subscribe_params* params, const void* data, uint16_t length) {
    if (data == NULL) {
        return BT_GATT_ITER_STOP;
    }

    uint8_t conn_idx = bt_conn_index(conn);
    switch2_pro_client* client = &switch2_pro_clients[conn_idx];
    const uint8_t* value = (const uint8_t*) data;
    if (length < 4) {
        return BT_GATT_ITER_CONTINUE;
    }

    uint8_t cmd = value[0];
    uint8_t sub = value[3];
    LOG_INF("switch2_pro ack cmd=0x%02x sub=0x%02x state=%u len=%u", cmd, sub, (uint8_t) client->init_state, length);
    switch2_flight_record(Switch2FlightEvent::BLE_INIT, cmd, sub, (uint8_t) client->init_state, (uint8_t) MIN(length, (uint16_t) 255), value, length);
    client->init_cmd_in_flight = false;

    if (cmd == 0x02 && client->init_state == Switch2ProInitState::READ_INFO) {
        client->init_state = Switch2ProInitState::PAIR_STEP1;
    } else if (cmd == 0x15 && sub == 0x01 && client->init_state == Switch2ProInitState::PAIR_STEP1) {
        client->init_state = Switch2ProInitState::PAIR_STEP2;
    } else if (cmd == 0x15 && sub == 0x04 && client->init_state == Switch2ProInitState::PAIR_STEP2) {
        client->init_state = Switch2ProInitState::PAIR_STEP3;
    } else if (cmd == 0x15 && sub == 0x02 && client->init_state == Switch2ProInitState::PAIR_STEP3) {
        client->init_state = Switch2ProInitState::PAIR_STEP4;
    } else if (cmd == 0x15 && sub == 0x03 && client->init_state == Switch2ProInitState::PAIR_STEP4) {
        client->init_state = Switch2ProInitState::SET_LED;
    } else if (cmd == 0x09 && client->init_state == Switch2ProInitState::SET_LED) {
        client->init_state = Switch2ProInitState::DONE;
        LOG_INF("switch2_pro init done");
        k_work_cancel_delayable(&switch2_pro_init_retry_work);
    }
    if (client->init_state != Switch2ProInitState::DONE) {
        k_work_reschedule(&switch2_pro_init_retry_work, K_MSEC(500));
    }

    return BT_GATT_ITER_CONTINUE;
}

static void switch2_pro_mtu_exchange_cb(struct bt_conn* conn, uint8_t err, struct bt_gatt_exchange_params* params) {
    uint8_t conn_idx = bt_conn_index(conn);
    switch2_pro_client* client = &switch2_pro_clients[conn_idx];
    if (err) {
        LOG_WRN("switch2_pro mtu exchange err=%u state=%u mtu=%u", err, (uint8_t) client->init_state, bt_gatt_get_mtu(conn));
    } else {
        LOG_INF("switch2_pro mtu exchange ok mtu=%u", bt_gatt_get_mtu(conn));
    }
    k_work_reschedule(&switch2_pro_init_retry_work, K_MSEC(500));
}

static bool switch2_pro_write_cmd(switch2_pro_client* client, const uint8_t* data, uint16_t len) {
    if (client == NULL || client->conn == NULL || client->cmd_value_handle == 0) {
        return false;
    }
    int err = bt_gatt_write_without_response(client->conn, client->cmd_value_handle, data, len, false);
    if (err) {
        LOG_WRN("switch2_pro cmd write failed err=%d state=%u", err, (uint8_t) client->init_state);
        return false;
    }
    LOG_INF("switch2_pro cmd write state=%u len=%u cmd=0x%02x sub=0x%02x", (uint8_t) client->init_state, len, data[0], len > 3 ? data[3] : 0);
    switch2_flight_record(Switch2FlightEvent::BLE_INIT, data[0], len > 3 ? data[3] : 0, (uint8_t) client->init_state, (uint8_t) MIN(len, (uint16_t) 255), data, len);
    client->init_cmd_in_flight = true;
    client->init_cmd_sent_at = k_uptime_get();
    return true;
}

static void switch2_pro_send_init_cmd(switch2_pro_client* client) {
    switch (client->init_state) {
        case Switch2ProInitState::READ_INFO: {
            uint8_t cmd[] = { 0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x40, 0x7e, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00 };
            switch2_pro_write_cmd(client, cmd, sizeof(cmd));
            break;
        }
        case Switch2ProInitState::PAIR_STEP1: {
            bt_addr_le_t local_addr;
            size_t count = 1;
            bt_id_get(&local_addr, &count);
            uint8_t cmd[] = {
                0x15, 0x91, 0x01, 0x01, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x02,
                local_addr.a.val[0], local_addr.a.val[1], local_addr.a.val[2], local_addr.a.val[3], local_addr.a.val[4], local_addr.a.val[5],
                (uint8_t) (local_addr.a.val[0] - 1), local_addr.a.val[1], local_addr.a.val[2], local_addr.a.val[3], local_addr.a.val[4], local_addr.a.val[5],
            };
            switch2_pro_write_cmd(client, cmd, sizeof(cmd));
            break;
        }
        case Switch2ProInitState::PAIR_STEP2: {
            uint8_t cmd[] = {
                0x15, 0x91, 0x01, 0x04, 0x00, 0x11, 0x00, 0x00, 0x00,
                0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42, 0xc6,
                0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31,
            };
            switch2_pro_write_cmd(client, cmd, sizeof(cmd));
            break;
        }
        case Switch2ProInitState::PAIR_STEP3: {
            uint8_t cmd[] = {
                0x15, 0x91, 0x01, 0x02, 0x00, 0x11, 0x00, 0x00, 0x00,
                0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b, 0x41,
                0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73,
            };
            switch2_pro_write_cmd(client, cmd, sizeof(cmd));
            break;
        }
        case Switch2ProInitState::PAIR_STEP4: {
            uint8_t cmd[] = { 0x15, 0x91, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00 };
            switch2_pro_write_cmd(client, cmd, sizeof(cmd));
            break;
        }
        case Switch2ProInitState::SET_LED: {
            uint8_t cmd[] = { 0x09, 0x91, 0x01, 0x07, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            switch2_pro_write_cmd(client, cmd, sizeof(cmd));
            break;
        }
        default:
            break;
    }
}

static void switch2_pro_init_retry_work_fn(struct k_work* work) {
    bool retry_needed = false;
    for (uint8_t i = 0; i < CONFIG_BT_MAX_CONN; i++) {
        switch2_pro_client* client = &switch2_pro_clients[i];
        if (client->conn == NULL ||
            client->init_state == Switch2ProInitState::IDLE ||
            client->init_state == Switch2ProInitState::DONE) {
            continue;
        }
        retry_needed = true;
        if (client->init_cmd_in_flight &&
            k_uptime_get() - client->init_cmd_sent_at < SWITCH2_PRO_INIT_ACK_TIMEOUT_MS) {
            continue;
        }
        if (client->init_cmd_in_flight) {
            LOG_WRN("switch2_pro init ack timeout state=%u", (uint8_t) client->init_state);
            client->init_cmd_in_flight = false;
        }
        LOG_INF("switch2_pro retry init state=%u", (uint8_t) client->init_state);
        switch2_pro_send_init_cmd(client);
    }
    if (retry_needed) {
        k_work_reschedule(&switch2_pro_init_retry_work, K_MSEC(1000));
    }
}

static void switch2_pro_connect_att(struct bt_conn* conn) {
    uint8_t conn_idx = bt_conn_index(conn);
    switch2_pro_client* client = &switch2_pro_clients[conn_idx];
    memset(client, 0, sizeof(*client));
    client->conn = bt_conn_ref(conn);
    client->conn_idx = conn_idx;
    client->input_value_handle = SWITCH2_PRO_INPUT_VALUE_HANDLE;
    client->output_value_handle = SWITCH2_PRO_OUTPUT_VALUE_HANDLE;
    client->cmd_value_handle = SWITCH2_PRO_CMD_VALUE_HANDLE;
    client->init_state = Switch2ProInitState::READ_INFO;

    memset(&client->ack_subscribe_params, 0, sizeof(client->ack_subscribe_params));
    client->ack_subscribe_params.notify = switch2_pro_ack_notify_cb;
    client->ack_subscribe_params.value = BT_GATT_CCC_NOTIFY;
    client->ack_subscribe_params.value_handle = SWITCH2_PRO_ACK_VALUE_HANDLE;
    client->ack_subscribe_params.ccc_handle = SWITCH2_PRO_ACK_CCC_HANDLE;

    int err = bt_gatt_subscribe(conn, &client->ack_subscribe_params);
    if (err && err != -EALREADY) {
        LOG_WRN("switch2_pro ack subscribe err=%d", err);
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
        return;
    }

    memset(&client->input_subscribe_params, 0, sizeof(client->input_subscribe_params));
    client->input_subscribe_params.notify = switch2_pro_notify_cb;
    client->input_subscribe_params.value = BT_GATT_CCC_NOTIFY;
    client->input_subscribe_params.value_handle = SWITCH2_PRO_INPUT_VALUE_HANDLE;
    client->input_subscribe_params.ccc_handle = SWITCH2_PRO_INPUT_CCC_HANDLE;

    err = bt_gatt_subscribe(conn, &client->input_subscribe_params);
    if (err && err != -EALREADY) {
        LOG_WRN("switch2_pro input subscribe err=%d", err);
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
        return;
    }

    LOG_INF("switch2_pro direct ATT subscribed input=0x%04x ack=0x%04x", SWITCH2_PRO_INPUT_VALUE_HANDLE, SWITCH2_PRO_ACK_VALUE_HANDLE);
    switch_pro_input_enabled = true;
    switch_pro_hogp_ready_events++;
    device_connected_callback(conn_idx << 8, 0x057e, 0x2069, 0);
    memset(&client->mtu_exchange_params, 0, sizeof(client->mtu_exchange_params));
    client->mtu_exchange_params.func = switch2_pro_mtu_exchange_cb;
    err = bt_gatt_exchange_mtu(conn, &client->mtu_exchange_params);
    if (err) {
        LOG_WRN("switch2_pro mtu exchange start err=%d mtu=%u", err, bt_gatt_get_mtu(conn));
        k_work_reschedule(&switch2_pro_init_retry_work, K_MSEC(1500));
    }
}

static int64_t button_pressed_at;

static void button_cb(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
    int button_state = gpio_pin_get(dev, button.pin);
    if (button_state) {
        button_pressed_at = k_uptime_get();
    } else {
        if (k_uptime_get() - button_pressed_at > CLEAR_BONDS_BUTTON_PRESS_MS) {
            clear_bonds();
        } else {
            pair_new_device();
        }
    }
}

static void connected(struct bt_conn* conn, uint8_t conn_err) {
    char addr[BT_ADDR_LE_STR_LEN];

    scanning = false;
    int conn_count = count_connections();
    if (is_switch_pro_mode() && conn_count > (int) switch_pro_conn_count_highwater) {
        switch_pro_conn_count_highwater = conn_count;
    }
    set_led_mode(LedMode::BLINK);

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (conn_err) {
        LOG_ERR("Failed to connect to %s (conn_err=%u).", addr, conn_err);
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));

        return;
    }

    LOG_INF("%s", addr);

    if (is_switch_pro_mode()) {
        switch_pro_bt_connected_events++;
    }
    switch2_flight_record(Switch2FlightEvent::BT_CONNECT, conn_err, 0, 0, 0);

    uint8_t conn_idx = bt_conn_index(conn);
    if (pending_switch2_pro_valid && bt_addr_le_eq(bt_conn_get_dst(conn), &pending_switch2_pro_addr)) {
        pending_switch2_pro_valid = false;
        conn_kinds[conn_idx] = ConnKind::SWITCH2_PRO;
        LOG_INF("Switch 2 Pro connected; starting direct ATT setup");
        switch2_flight_record(Switch2FlightEvent::BT_CONNECT, conn_err, conn_idx, (uint8_t) ConnKind::SWITCH2_PRO, 0);
        switch2_pro_connect_att(conn);
        return;
    }

    conn_kinds[conn_idx] = ConnKind::HOGP;
    CHK(bt_conn_set_security(conn, BT_SECURITY_L2));
}

static void disconnected(struct bt_conn* conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("%s (reason=%u)", addr, reason);

    if (is_switch_pro_mode()) {
        switch_pro_bt_disconnected_events++;
        switch_pro_last_disconnect_reason = reason;
    }
    switch2_flight_record(Switch2FlightEvent::BT_DISCONNECT, reason, bt_conn_index(conn), (uint8_t) conn_kinds[bt_conn_index(conn)], 0);

    uint8_t conn_idx = bt_conn_index(conn);

    if (conn_kinds[conn_idx] == ConnKind::SWITCH2_PRO) {
        switch2_pro_client* client = &switch2_pro_clients[conn_idx];
        if (client->conn != NULL) {
            bt_gatt_unsubscribe(conn, &client->input_subscribe_params);
            bt_gatt_unsubscribe(conn, &client->ack_subscribe_params);
            bt_conn_unref(client->conn);
        }
        memset(client, 0, sizeof(*client));
    }
    conn_kinds[conn_idx] = ConnKind::NONE;

    if (bt_hogp_assign_check(&hogps[conn_idx])) {
        bt_hogp_release(&hogps[conn_idx]);
    }

    struct disconnected_type disconnected_item = { .conn_idx = conn_idx };
    CHK(k_msgq_put(&disconnected_q, &disconnected_item, K_NO_WAIT));

    count_connections();

    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}

static void security_changed(struct bt_conn* conn, bt_security_t level, enum bt_security_err err) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        LOG_INF("%s, level=%u.", addr, level);
        switch2_flight_record(Switch2FlightEvent::BT_SECURITY, level, 0, (uint8_t) conn_kinds[bt_conn_index(conn)], 0);
        peers_only = true;
        gatt_discover(conn);
    } else {
        LOG_ERR("security failed: %s, level=%u, err=%d", addr, level, err);
        switch2_flight_record(Switch2FlightEvent::BT_SECURITY, level, err, (uint8_t) conn_kinds[bt_conn_index(conn)], 0);
    }
}

static void le_param_updated(struct bt_conn* conn, uint16_t interval, uint16_t latency, uint16_t timeout) {
    LOG_INF("interval=%u, latency=%u, timeout=%u", interval, latency, timeout);
}

static bool le_param_req(struct bt_conn* conn, struct bt_le_conn_param* param) {
    LOG_INF("interval_min=%d, interval_max=%d, latency=%d, timeout=%d", param->interval_min, param->interval_max, param->latency, param->timeout);
    param->interval_max = param->interval_min;
    return true;
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_req = le_param_req,
    .le_param_updated = le_param_updated,
    .security_changed = security_changed,
};

static void scan_init() {
    struct bt_scan_init_param scan_init = {
        .scan_param = NULL,
        .connect_if_match = 1,
        .conn_param = conn_param,
    };

    bt_scan_init(&scan_init);
    bt_scan_cb_register(&scan_cb);
}

static int8_t hogp_index(struct bt_hogp* hogp) {
    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
        if (&hogps[i] == hogp) {
            return i;
        }
    }

    LOG_ERR("unknown hogp!");
    return -1;
}

static uint8_t hogp_notify_cb(struct bt_hogp* hogp, struct bt_hogp_rep_info* rep, uint8_t err, const uint8_t* data) {
    k_work_reschedule(&activity_led_off_work, K_MSEC(50));  // XXX what if work_fn is currently running? it might turn the led off after we turn it on
    gpio_pin_set_dt(&led0, true);

    if (!data) {
        return BT_GATT_ITER_STOP;
    }

    if (scanning) {
        scanning = false;  // more reports can come in before we actually stop scanning; there's probably a scenario where this causes trouble though
        k_work_submit(&scan_stop_work);
    } else {
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
    }

    static struct report_type buf;
    buf.interface = hogp_index(hogp) << 8;
    buf.len = bt_hogp_rep_size(rep) + 1;
    buf.data[0] = bt_hogp_rep_id(rep);

    memcpy(buf.data + 1, data, buf.len);
    if (k_msgq_put(&report_q, &buf, K_NO_WAIT)) {
        if (is_switch_pro_mode()) {
            switch_pro_ble_report_drops++;
        }
    } else if (is_switch_pro_mode()) {
        switch_pro_ble_reports++;
        note_switch_pro_report_q_depth();
    }

    return BT_GATT_ITER_CONTINUE;
}

// XXX is this ready for simultaneous connection setup? is discovery ready? do we care?
static struct descriptor_type their_descriptor;

static void hogp_map_read_cb(struct bt_hogp* hogp, uint8_t err, const uint8_t* data, size_t size, size_t offset) {
    if (data == NULL) {
        their_descriptor.size = offset;
        their_descriptor.conn_idx = hogp_index(hogp);
        CHK(k_msgq_put(&descriptor_q, &their_descriptor, K_NO_WAIT));
        return;
    }

    memcpy(their_descriptor.data + offset, data, size);

    bt_hogp_map_read(hogp, hogp_map_read_cb, offset + size, K_NO_WAIT);
}

struct find_bond_t {
    bt_addr_le_t addr;
    uint8_t i;
    uint8_t found_idx;
};

static void find_bond_cb(const struct bt_bond_info* info, void* user_data) {
    struct find_bond_t* find_bond = (struct find_bond_t*) user_data;
    find_bond->i++;
    if (bt_addr_le_eq(&find_bond->addr, &info->addr)) {
        find_bond->found_idx = find_bond->i;
    }
}

static void hogp_ready_work_fn(struct k_work* work) {
    struct bt_hogp_rep_info* rep = NULL;
    struct hogp_ready_type item;

    while (!k_msgq_get(&hogp_ready_q, &item, K_NO_WAIT)) {
        LOG_INF("hogp_ready");
        if (is_switch_pro_mode()) {
            switch_pro_hogp_ready_events++;
        }

        struct find_bond_t find_bond = {
            .i = 0,
            .found_idx = 0,
        };
        bt_addr_le_copy(&find_bond.addr, bt_conn_get_dst(bt_hogp_conn(item.hogp)));
        bt_foreach_bond(BT_ID_DEFAULT, find_bond_cb, &find_bond);
        LOG_DBG("found bond idx: %d", find_bond.found_idx);
        device_connected_callback(bt_conn_index(bt_hogp_conn(item.hogp)) << 8, 1, 1, find_bond.found_idx);

        while (NULL != (rep = bt_hogp_rep_next(item.hogp, rep))) {
            if (bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_INPUT) {
                LOG_DBG("subscribing to report ID: %u", bt_hogp_rep_id(rep));
                CHK(bt_hogp_rep_subscribe(item.hogp, rep, hogp_notify_cb));
            }
        }

        bt_hogp_map_read(item.hogp, hogp_map_read_cb, 0, K_NO_WAIT);

        if (is_switch_pro_mode()) {
            // Short connect buzz: medium strength for 250 ms.
            switch_pro_rumble_to_hogp(item.hogp, 0x60, 0x40);
            k_work_reschedule(&connect_rumble_stop_work, K_MSEC(250));
        }
    }
}
static K_WORK_DEFINE(hogp_ready_work, hogp_ready_work_fn);

static void hogp_ready_cb(struct bt_hogp* hogp) {
    struct hogp_ready_type q_item = { .hogp = hogp };
    CHK(k_msgq_put(&hogp_ready_q, &q_item, K_NO_WAIT));
    k_work_submit(&hogp_ready_work);
}

static void hogp_prep_error_cb(struct bt_hogp* hogp, int err) {
    LOG_ERR("err=%d", err);
}

static const struct bt_hogp_init_params hogp_init_params = {
    .ready_cb = hogp_ready_cb,
    .prep_error_cb = hogp_prep_error_cb,
};

static void auth_cancel(struct bt_conn* conn) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_WRN("%s", addr);
}

static void pairing_complete(struct bt_conn* conn, bool bonded) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("%s, bonded=%d", addr, bonded);
    switch2_flight_record(Switch2FlightEvent::BT_PAIRING, bonded, 0, 0, 0);
    if (bonded && is_switch2_pro_ble_mode()) {
        switch2_bond_keys_snapshot_now();
    }
}

static void pairing_failed(struct bt_conn* conn, enum bt_security_err reason) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_ERR("%s, reason %d", addr, reason);
    switch2_flight_record(Switch2FlightEvent::BT_PAIRING, 0xff, reason, 0, 0);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed
};

static int set_report_cb(const struct device* dev, struct usb_setup_packet* setup, int32_t* len, uint8_t** data) {
    uint8_t request_value[2];

    // report_id, report_type
    sys_put_le16(setup->wValue, request_value);

    printk("USB_SETUP bm=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=0x%04x status=SET_REPORT\n",
           setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, (unsigned)*len);
    LOG_INF("report_id=%d, report_type=%d, len=%d", request_value[0], request_value[1], *len);
    LOG_HEXDUMP_DBG((*data), (uint32_t) *len, "");
    if (dev == hid_dev0 && is_switch2_pro_mode()) {
        switch2_flight_record(Switch2FlightEvent::SET_REPORT, request_value[0], request_value[1], (uint8_t) MIN(*len, (int32_t) 255), 0, *data, (uint8_t) MIN(*len, (int32_t) 255));
    }

    struct set_report_type buf;
    if ((request_value[0] > 0) && (*len > 0)) {
        bool config_report = request_value[0] == REPORT_ID_CONFIG && (dev == hid_dev1 || (dev == hid_dev0 && is_switch2_pro_mode()));
        if ((dev == hid_dev0) && !config_report && set_report0_synchronous(request_value[0])) {
            handle_set_report0(request_value[0], (*data) + 1, (*len) - 1);
        } else {
            if (config_report) {
                buf.interface = 1;
                k_mutex_lock(&get_report_mutex, K_FOREVER);
                get_report_response_ready = false;
                k_mutex_unlock(&get_report_mutex);
            } else if (dev == hid_dev0) {
                buf.interface = 0;
            } else if (dev == hid_dev1) {
                buf.interface = 1;
                k_mutex_lock(&get_report_mutex, K_FOREVER);
                get_report_response_ready = false;
                k_mutex_unlock(&get_report_mutex);
            }
            buf.report_id = request_value[0];
            buf.len = *len - 1;
            memcpy(buf.data, (*data) + 1, (*len) - 1);
            CHK(k_msgq_put(&set_report_q, &buf, K_NO_WAIT));
        }
    } else {
        LOG_ERR("no report ID?");
    }

    return 0;
};

static int get_report_cb(const struct device* dev, struct usb_setup_packet* setup, int32_t* len, uint8_t** data) {
    uint8_t request_value[2];

    sys_put_le16(setup->wValue, request_value);

    printk("USB_SETUP bm=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=0x%04x status=GET_REPORT\n",
           setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, (unsigned)*len);
    LOG_INF("report_id=%d, %d, len=%d", request_value[0], request_value[1], *len);
    if (dev == hid_dev0 && is_switch2_pro_mode()) {
        switch2_flight_record(Switch2FlightEvent::GET_REPORT, request_value[0], request_value[1], (uint8_t) MIN(*len, (int32_t) 255), 0);
    }

    *data[0] = request_value[0];
    bool config_report = request_value[0] == REPORT_ID_CONFIG && (dev == hid_dev1 || (dev == hid_dev0 && is_switch2_pro_mode()));
    if (config_report) {
        k_mutex_lock(&get_report_mutex, K_FOREVER);
        if (get_report_response_ready) {
            memcpy((*data) + 1, get_report_buf, CONFIG_SIZE);
            *len = CONFIG_SIZE;
        } else {
            LOG_INF("response not ready");
            *len = 0;
        }
        get_report_response_ready = false;
        k_mutex_unlock(&get_report_mutex);
    } else if (dev == hid_dev0) {
        *len = handle_get_report0(request_value[0], (*data) + 1, CONFIG_SIZE);
    }
    (*len)++;

    return 0;
};

static void int_in_ready_cb0(const struct device* dev) {
    k_sem_give(&usb_sem0);
    // LOOP DEBUG: fires when the console polls + reads our IN endpoint. If this
    // never logs, the console isn't polling us (descriptor-level non-acceptance).
    static uint32_t in_polls = 0;
    static int64_t last_in_log_ms = 0;
    in_polls++;
    int64_t now = k_uptime_get();
    if (now - last_in_log_ms >= 1000) {
        last_in_log_ms = now;
        LOG_INF("sw2 IN endpoint polled by console: count=%u", in_polls);
    }
}

static void int_out_ready_cb0(const struct device* dev) {
    static struct report_type buf;
    uint32_t len;
    if (CHK(hid_int_ep_read(hid_dev0, buf.data, sizeof(buf.data), &len))) {
        if (is_switch2_pro_mode()) {
            switch2_flight_record(Switch2FlightEvent::INT_OUT, buf.data[0], len > 1 ? buf.data[1] : 0, len > 2 ? buf.data[2] : 0, len > 3 ? buf.data[3] : 0, buf.data, (uint8_t) MIN(len, (uint32_t) 255));
        }
        if (is_switch_pro_mode() && switch_pro_handle_output_report(buf.data, len)) {
            switch_pro_host_reports++;
            return;
        }

        buf.interface = OUR_OUT_INTERFACE;
        buf.len = len;
        if (k_msgq_put(&report_q, &buf, K_NO_WAIT)) {
            if (is_switch_pro_mode()) {
                switch_pro_host_report_drops++;
            }
        } else if (is_switch_pro_mode()) {
            switch_pro_host_reports++;
            note_switch_pro_report_q_depth();
        }
    }
}

static void int_in_ready_cb1(const struct device* dev) {
    k_sem_give(&usb_sem1);
}

#if CONFIG_USB_HID_DEVICE_COUNT == 1
static void switch2_vendor_ep_cb(uint8_t ep, enum usb_dc_ep_cb_status_code cb_status) {
    if (ep == SWITCH2_VENDOR_IN_EP && cb_status == USB_DC_EP_DATA_IN) {
        k_sem_give(&switch2_vendor_in_sem);
        return;
    }

    if (ep != SWITCH2_VENDOR_OUT_EP || cb_status != USB_DC_EP_DATA_OUT) {
        return;
    }

    static uint8_t buf[64];
    uint32_t len = 0;
    if (!CHK(usb_read(SWITCH2_VENDOR_OUT_EP, buf, sizeof(buf), &len)) || len == 0) {
        return;
    }

    printk("USB_EP_OUT ep=0x02 len=%u data=%02x%02x%02x%02x\n",
           (unsigned)len, buf[0], len>1?buf[1]:0, len>2?buf[2]:0, len>3?buf[3]:0);
    switch2_flight_record(Switch2FlightEvent::INT_OUT, buf[0], len > 1 ? buf[1] : 0, len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0, buf, (uint8_t) MIN(len, (uint32_t) 255));
    if (switch2_pro_handle_output_report(buf, (uint8_t) MIN(len, (uint32_t) 255))) {
        switch_pro_host_reports++;
    } else {
        switch_pro_host_report_drops++;
    }
}

// USB Interface Association Descriptor (IAD) — the real Switch 2 Pro groups its
// interfaces into 3 IADs; a composite (class 0xEF) device must emit them.
struct sw2_iad {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bFirstInterface;
    uint8_t bInterfaceCount;
    uint8_t bFunctionClass;
    uint8_t bFunctionSubClass;
    uint8_t bFunctionProtocol;
    uint8_t iFunction;
} __packed;

struct switch2_vendor_config {
    struct sw2_iad iad;
    struct usb_if_descriptor if0;
    struct usb_ep_descriptor out_ep;
    struct usb_ep_descriptor in_ep;
} __packed;

USBD_CLASS_DESCR_DEFINE(primary, 1) struct switch2_vendor_config switch2_vendor_desc = {
    .iad = {  // IAD#2: groups interface 1 (vendor) — 08 0b 01 01 ff 00 00 00
        .bLength = sizeof(struct sw2_iad),
        .bDescriptorType = 0x0b,  // USB_DESC_INTERFACE_ASSOC
        .bFirstInterface = 1,
        .bInterfaceCount = 1,
        .bFunctionClass = 0xff,
        .bFunctionSubClass = 0,
        .bFunctionProtocol = 0,
        .iFunction = 0,
    },
    .if0 = {
        .bLength = sizeof(struct usb_if_descriptor),
        .bDescriptorType = USB_DESC_INTERFACE,
        // Must be 0 at link time. usb_fix_descriptor only calls usb_get_cfg_data
        // (to locate our ep_cfg and validate endpoints) when bInterfaceNumber==0.
        // The interface_config callback then updates it to the correct runtime value.
        .bInterfaceNumber = 0,
        .bAlternateSetting = 0,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_BCC_VENDOR,
        .bInterfaceSubClass = 0,
        .bInterfaceProtocol = 0,
        .iInterface = 0,
    },
    .out_ep = {
        .bLength = sizeof(struct usb_ep_descriptor),
        .bDescriptorType = USB_DESC_ENDPOINT,
        .bEndpointAddress = SWITCH2_VENDOR_OUT_EP,
        .bmAttributes = USB_DC_EP_BULK,
        .wMaxPacketSize = sys_cpu_to_le16(SWITCH2_VENDOR_EP_MPS),
        .bInterval = 0,
    },
    .in_ep = {
        .bLength = sizeof(struct usb_ep_descriptor),
        .bDescriptorType = USB_DESC_ENDPOINT,
        .bEndpointAddress = SWITCH2_VENDOR_IN_EP,
        .bmAttributes = USB_DC_EP_BULK,
        .wMaxPacketSize = sys_cpu_to_le16(SWITCH2_VENDOR_EP_MPS),
        .bInterval = 0,
    },
};

static struct usb_ep_cfg_data switch2_vendor_ep_cfg[] = {
    {
        .ep_cb = switch2_vendor_ep_cb,
        .ep_addr = SWITCH2_VENDOR_OUT_EP,
    },
    {
        .ep_cb = switch2_vendor_ep_cb,
        .ep_addr = SWITCH2_VENDOR_IN_EP,
    },
};

static void switch2_vendor_interface_config(struct usb_desc_header* head, uint8_t bInterfaceNumber) {
    ARG_UNUSED(head);
    switch2_vendor_desc.if0.bInterfaceNumber = bInterfaceNumber;
}

// Identity response for vendor IN request 0x03 (wLength=64).
// Captured from a real Switch 2 Pro Controller connected to the console.
// Layout: [0]=0x01, [1]=0x00, [2..15]=serial(14 bytes), [16..17]=\0\0,
// [18..19]=VID LE, [20..21]=PID LE, [22..23]=version, [24]=unk,
// [25..27]=grip-L color, [28..30]=body color, [31..33]=button color,
// [34..36]=grip-R color, [37..63]=0xff padding.
static const uint8_t sw2_vendor_req03_resp[64] = {
    0x01, 0x00,
    // Real controller serial from GreatFET capture — fill from BLE-connected controller later
    'H','E','W','7','0','0','0','6','1','6','9','7','8','0', 0x00, 0x00,
    0x7e, 0x05,  // VID = 0x057e (Nintendo)
    0x69, 0x20,  // PID = 0x2069 (Switch 2 Pro)
    0x01, 0x06,  // device version
    0x01,        // unknown
    // Real controller colors from capture
    0x23, 0x23, 0x23,  // grip L color
    0xa0, 0xa0, 0xa0,  // body color
    0xe6, 0xe6, 0xe6,  // button color
    0x32, 0x32, 0x32,  // grip R color
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

// Protocol/version response for vendor IN request 0x02 (wLength=16).
// Captured from real controller; last 6 bytes are a controller BT MAC.
static const uint8_t sw2_vendor_req02_resp[16] = {
    0x02, 0x01, 0x04, 0x00, 0x00, 0x00, 0x0c, 0x00,
    0x02, 0x03,
    // Real controller MAC in little-endian: 3c:a9:ab:69:17:3d — fill from BLE later
    0x3d, 0x17, 0x69, 0xab, 0xa9, 0x3c,
};

static int switch2_vendor_handler(struct usb_setup_packet* setup, int32_t* len, uint8_t** data) {
    if (setup->bmRequestType == 0xC0 && setup->bRequest == 0x03 && setup->wLength >= 64) {
        *len = 64;
        memcpy(*data, sw2_vendor_req03_resp, 64);
        printk("USB_SETUP bm=0xc0 req=0x03 status=identity_ACK\n");
        return 0;
    }
    if (setup->bmRequestType == 0xC0 && setup->bRequest == 0x02 && setup->wLength >= 16) {
        *len = 16;
        memcpy(*data, sw2_vendor_req02_resp, 16);
        printk("USB_SETUP bm=0xc0 req=0x02 status=proto_ACK\n");
        return 0;
    }
    // bm=0x40 = vendor OUT to device; req=0x04 appears after identity exchange —
    // console expects zero-length ACK before it sends the BULK OUT 0x91.
    if (setup->bmRequestType == 0x40 && setup->bRequest == 0x04 && setup->wLength == 0) {
        *len = 0;
        printk("USB_SETUP bm=0x40 req=0x04 val=0x%04x status=set_ACK\n", setup->wValue);
        return 0;
    }
    printk("USB_SETUP bm=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=0x%04x status=vendor_STALL\n",
           setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, (unsigned)setup->wLength);
    ARG_UNUSED(data);
    return -ENOTSUP;
}

USBD_CFG_DATA_DEFINE(primary, switch2_vendor) struct usb_cfg_data switch2_vendor_config = {
    .usb_device_description = NULL,
    .interface_descriptor = &switch2_vendor_desc.if0,
    .interface_config = switch2_vendor_interface_config,
    .cb_usb_status = NULL,
    .interface = {
        .class_handler = NULL,
        .vendor_handler = switch2_vendor_handler,
        .custom_handler = NULL,
    },
    .num_endpoints = ARRAY_SIZE(switch2_vendor_ep_cfg),
    .endpoint = switch2_vendor_ep_cfg,
};

// IAD#3 + the audio function (interfaces 2,3,4) — raw bytes copied verbatim from
// a real Switch 2 Pro's config descriptor (switch2-usb-re/captures/
// real_sw2pro_descriptors.txt, offset 0x50..0x10c). Emitted at section order 2
// so it lands after the vendor interface. The isochronous audio endpoints
// (0x03/0x83) are declared in the descriptor but not backed by ep cfg — the
// console only opens them if it uses the mic, which it doesn't on the
// controller screen. Purpose: make our composite structure match the real one
// so the console recognizes us as a Switch 2 Pro.
// Audio descriptor: IAD#3 + IF2 (AudioControl) + IF3 alt=0 + IF4 alt=0, all with
// bNumEndpoints=0. The real controller also has alt=1 for IF3/IF4 with isochronous
// EP 0x03/0x83, but Zephyr's usb_fix_descriptor rejects any endpoint in the
// descriptor that has no registered ep_cfg — which caused "Failed to validate
// endpoints" and prevented USB from initialising entirely. Omitting alt=1 means
// the console can't open the audio stream, but it never does on the controller
// screen so this is fine. The 5-interface shape (IF0-IF4) still matches.
USBD_CLASS_DESCR_DEFINE(primary, 2) uint8_t sw2_audio_desc[] = {
    0x08, 0x0b, 0x02, 0x03, 0x01, 0x01, 0x00, 0x00,             // IAD#3 (IF2-4 audio)
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,       // IF2 AudioControl alt0 0EP
    0x0a, 0x24, 0x01, 0x00, 0x01, 0x47, 0x00, 0x02, 0x03, 0x04, // CS AC header
    0x0c, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x0a, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x24, 0x03, 0x03, 0x02, 0x03, 0x00, 0x02, 0x00,
    0x0c, 0x24, 0x02, 0x04, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x09, 0x24, 0x06, 0x05, 0x04, 0x01, 0x03, 0x00, 0x00,
    0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x00, 0x05, 0x00,
    0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,       // IF3 AudioStreaming alt0 0EP
    0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,       // IF4 AudioStreaming alt0 0EP
};

// IAD#1: groups interface 0 (HID) — 08 0b 00 01 03 00 00 00. Placed at section
// order 0 (same as the auto-generated HID interface) to try to land before it.
USBD_CLASS_DESCR_DEFINE(primary, 0) struct sw2_iad sw2_iad1 = {
    .bLength = sizeof(struct sw2_iad),
    .bDescriptorType = 0x0b,
    .bFirstInterface = 0,
    .bInterfaceCount = 1,
    .bFunctionClass = 0x03,  // HID
    .bFunctionSubClass = 0,
    .bFunctionProtocol = 0,
    .iFunction = 0,
};
#endif

static const struct hid_ops ops0 = {
    .get_report = get_report_cb,
    .set_report = set_report_cb,
    .int_in_ready = int_in_ready_cb0,
    .int_out_ready = int_out_ready_cb0,
};

static const struct hid_ops ops1 = {
    .get_report = get_report_cb,
    .set_report = set_report_cb,
    .int_in_ready = int_in_ready_cb1,
};

static bool do_send_report(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    if (is_switch2_pro_mode() && interface == 0 && len > 0 && report_with_id[0] == 0x09) {
        memcpy(switch_pro_current_input, report_with_id, MIN((uint8_t) sizeof(switch_pro_current_input), len));
        switch_pro_current_input[1] = switch_pro_timer++;
        switch_pro_current_input[12] = (switch2_pro_feature_flags & 0x20) ? 0x38 : 0x30;
        switch_pro_last_input_ms = k_uptime_get();
        bool sent = CHK(hid_int_ep_write(hid_dev0, switch_pro_current_input, sizeof(switch_pro_current_input), NULL));
        switch2_flight_record_send_input(sent);
        if (sent) {
            switch_pro_mapped_writes++;
        } else {
            switch_pro_mapped_write_fails++;
        }
        return sent;
    }

    if (is_switch_pro_mode() && interface == 0 && len > 0 && report_with_id[0] == 0x30) {
        switch_pro_translate_report(report_with_id, len);
        switch_pro_current_input[1] = switch_pro_timer++;
        switch_pro_last_input_ms = k_uptime_get();
        bool sent = CHK(hid_int_ep_write(hid_dev0, switch_pro_current_input, sizeof(switch_pro_current_input), NULL));
        if (is_switch2_pro_mode()) {
            switch2_flight_record_send_input(sent);
        }
        if (sent) {
            switch_pro_mapped_writes++;
        } else {
            switch_pro_mapped_write_fails++;
        }
        return sent;
    }

    if (report_with_id[0] == 0) {
        report_with_id++;
        len--;
    }
    if (interface == 0) {
        return CHK(hid_int_ep_write(hid_dev0, report_with_id, len, NULL));
    }
    if (interface == 1) {
        return CHK(hid_int_ep_write(hid_dev1, report_with_id, len, NULL));
    }
    return false;
}

static void button_init() {
    if (!device_is_ready(button.port)) {
        LOG_ERR("button device %s is not ready", button.port->name);
        return;
    }

    if (!CHK(gpio_pin_configure_dt(&button, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW))) {
        return;
    }

    if (!CHK(gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH))) {
        return;
    }

    gpio_init_callback(&button_cb_data, button_cb, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
}

static void leds_init() {
    if (device_is_ready(led0.port)) {
        CHK(gpio_pin_configure_dt(&led0, GPIO_OUTPUT));
        gpio_pin_set_dt(&led0, false);
    } else {
        LOG_ERR("led0 device %s is not ready", led0.port->name);
    }

    if (device_is_ready(led1.port)) {
        CHK(gpio_pin_configure_dt(&led1, GPIO_OUTPUT));
        gpio_pin_set_dt(&led1, false);
    } else {
        LOG_ERR("led1 device %s is not ready", led1.port->name);
    }
}

static void status_cb(enum usb_dc_status_code status, const uint8_t* param) {
    if (status == USB_DC_SOF) {
        atomic_set_bit(tick_pending, 0);
        return;
    }

    switch2_flight_record(Switch2FlightEvent::USB_STATUS, status, 0, 0, 0);
    if (status == USB_DC_RESET || status == USB_DC_CONFIGURED || status == USB_DC_SUSPEND) {
        if (is_switch_pro_mode()) {
            switch_pro_reset_session();
        }
    }
}

extern struct usb_device_descriptor __usb_descriptor_start[];

static void descriptor_init() {
    our_descriptor = &our_descriptors[our_descriptor_number];
    if ((our_descriptor->vid != 0) && (our_descriptor->pid != 0)) {
        struct usb_device_descriptor* device_descriptor = __usb_descriptor_start;
        device_descriptor->idVendor = our_descriptor->vid;
        device_descriptor->idProduct = our_descriptor->pid;
        if (is_switch2_pro_mode()) {
            device_descriptor->bDeviceClass = 0xef;
            device_descriptor->bDeviceSubClass = 0x02;
            device_descriptor->bDeviceProtocol = 0x01;
            device_descriptor->bcdDevice = sys_cpu_to_le16(0x0201);
        }
    }
}

static void usb_init() {
    hid_dev0 = device_get_binding("HID_0");
    if (hid_dev0 == NULL) {
        LOG_ERR("Cannot get USB HID Device 0.");
        return;
    }

    usb_hid_register_device(hid_dev0, our_descriptor->descriptor, our_descriptor->descriptor_length, &ops0);
#if CONFIG_USB_HID_DEVICE_COUNT > 1
    hid_dev1 = device_get_binding("HID_1");
    if (hid_dev1 == NULL) {
        LOG_ERR("Cannot get USB HID Device 1.");
        return;
    }
    usb_hid_register_device(hid_dev1, config_report_descriptor, config_report_descriptor_length, &ops1);
#endif
    CHK(usb_hid_init(hid_dev0));
#if CONFIG_USB_HID_DEVICE_COUNT > 1
    CHK(usb_hid_init(hid_dev1));
#endif
    CHK(usb_enable(status_cb));

    // LOOP DEBUG: dump the assembled USB descriptor blob (device + config + ...)
    // so we can diff our emitted config descriptor against the real Switch 2 Pro's.
    if (is_switch2_pro_mode()) {
        const uint8_t* d = (const uint8_t*) __usb_descriptor_start;
        for (int i = 0; i < 288; i += 16) {
            printk("usbdesc %03x:", i);
            for (int j = 0; j < 16; j++) {
                printk(" %02x", d[i + j]);
            }
            printk("\n");
            k_msleep(5);
        }
    }
}

static void bt_init() {
    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
        bt_hogp_init(&hogps[i], &hogp_init_params);
    }

    if (!CHK(bt_conn_auth_cb_register(&conn_auth_callbacks))) {
        return;
    }

    if (!CHK(bt_conn_auth_info_cb_register(&conn_auth_info_callbacks))) {
        return;
    }

    CHK(bt_enable(NULL));
}

static int remapper_settings_set(const char* name, size_t len, settings_read_cb read_cb, void* cb_arg) {
    LOG_INF("%s len=%d", name, len);

    if (!strcmp(name, "switch_pro_diag")) {
        if (len != sizeof(switch_pro_saved_diag)) {
            return -EINVAL;
        }

        int bytes_read = read_cb(cb_arg, switch_pro_saved_diag, len);
        if (bytes_read < 0) {
            return bytes_read;
        }
        return bytes_read == sizeof(switch_pro_saved_diag) ? 0 : -EINVAL;
    }

    if (!strcmp(name, "switch2_flight")) {
        if (len != sizeof(switch2_flight)) {
            return -EINVAL;
        }

        int bytes_read = read_cb(cb_arg, &switch2_flight, len);
        if (bytes_read < 0) {
            return bytes_read;
        }
        if (bytes_read != sizeof(switch2_flight)) {
            return -EINVAL;
        }
        switch2_flight_reset_if_invalid();
        return 0;
    }

    if (strcmp(name, "config")) {
        return -ENOENT;
    }

    static uint8_t buffer[PERSISTED_CONFIG_SIZE];

    if (len != PERSISTED_CONFIG_SIZE) {
        return -EINVAL;
    }

    int bytes_read = read_cb(cb_arg, buffer, len);

    if (bytes_read < 0) {
        return bytes_read;
    }

    if (bytes_read != PERSISTED_CONFIG_SIZE) {
        return -EINVAL;
    }

    //    LOG_HEXDUMP_DBG(buffer, len, "");

    load_config(buffer);

    return 0;
}

static struct settings_handler our_settings_handlers = {
    .name = "remapper",
    .h_set = remapper_settings_set,
};

void do_persist_config(uint8_t* buffer) {
    LOG_INF("");
    switch2_flight_record(Switch2FlightEvent::CONFIG_SET, our_descriptor_number, buffer[0], buffer[1], buffer[2], buffer, 8);
    CHK(settings_save_one("remapper/config", buffer, PERSISTED_CONFIG_SIZE));
}

// https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/master/src/main.c#L116
const int DFU_MAGIC_UF2_RESET = 0x57;

void reset_to_bootloader() {
    sys_reboot(DFU_MAGIC_UF2_RESET);
}

void flash_b_side() {
}

void get_switch_pro_diagnostics(uint32_t page, uint32_t values[7]) {
    if (page < SWITCH_PRO_DIAG_PAGES) {
        switch_pro_fill_diagnostics(page, values);
        return;
    }

    page -= SWITCH_PRO_DIAG_PAGES;
    if (page < SWITCH_PRO_DIAG_PAGES) {
        memcpy(values, switch_pro_saved_diag[page], SWITCH_PRO_DIAG_VALUES * sizeof(uint32_t));
        return;
    }

    memset(values, 0, SWITCH_PRO_DIAG_VALUES * sizeof(uint32_t));
}

void get_switch2_flight_log_page(uint32_t page, uint8_t out[28], uint8_t* out_len) {
    uint32_t offset = page * 28;
    if (offset >= sizeof(switch2_flight)) {
        *out_len = 0;
        return;
    }
    uint32_t remaining = sizeof(switch2_flight) - offset;
    *out_len = (uint8_t) (remaining < 28 ? remaining : 28);
    memcpy(out, (const uint8_t*) &switch2_flight + offset, *out_len);
}

void get_switch2_bond_keys_page(uint32_t page, uint8_t out[28], uint8_t* out_len) {
    if (page == 0) {
        switch2_bond_keys_snapshot_now();
    }

    uint32_t offset = page * 28;
    if (offset >= sizeof(switch2_bond_keys)) {
        *out_len = 0;
        return;
    }

    uint32_t remaining = sizeof(switch2_bond_keys) - offset;
    *out_len = (uint8_t) (remaining < 28 ? remaining : 28);
    memcpy(out, (const uint8_t*) &switch2_bond_keys + offset, *out_len);
}

void clear_switch2_flight_log() {
    memset(&switch2_flight, 0, sizeof(switch2_flight));
    switch2_flight.magic = SWITCH2_FLIGHT_MAGIC;
    switch2_flight.version = SWITCH2_FLIGHT_VERSION;
    switch2_flight_dirty = true;
    switch2_flight_persist(true);
}

void pair_new_device() {
    peers_only = false;
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}

void clear_bonds() {
    k_work_submit(&clear_bonds_work);
}

void my_mutexes_init() {
    for (int i = 0; i < (int8_t) MutexId::N; i++) {
        k_mutex_init(&mutexes[i]);
    }
    k_mutex_init(&get_report_mutex);
}

void my_mutex_enter(MutexId id) {
    k_mutex_lock(&mutexes[(uint8_t) id], K_FOREVER);
}

void my_mutex_exit(MutexId id) {
    k_mutex_unlock(&mutexes[(uint8_t) id]);
}

uint64_t get_time() {
    return k_uptime_get() * 1000;  // XXX precision?
}

void interval_override_updated() {
}

void queue_out_report(uint16_t interface, uint8_t report_id, const uint8_t* buffer, uint8_t len) {
    // TODO
}

void queue_set_feature_report(uint16_t interface, uint8_t report_id, const uint8_t* buffer, uint8_t len) {
    // TODO
}

void queue_get_feature_report(uint16_t interface, uint8_t report_id, uint8_t len) {
    // TODO
}

void set_gpio_inout_masks(uint32_t in_mask, uint32_t out_mask) {
}

int main() {
    LOG_INF("HID Remapper Bluetooth");

    my_mutexes_init();
    button_init();
    leds_init();
    CHK(settings_subsys_init());
    CHK(settings_register(&our_settings_handlers));
    bt_init();
    settings_load();
    switch2_flight_dump_saved();  // dump persisted console-handshake history to serial
    switch2_pro_ble_enabled = is_switch2_pro_mode();
#if CONFIG_USB_HID_DEVICE_COUNT == 1
    LOG_INF("Switch 2 console USB build: forcing Switch 2 Pro descriptor");
    our_descriptor_number = 7;
    switch2_pro_ble_enabled = true;
#endif
    descriptor_init();
    if (is_switch_pro_mode()) {
        switch_pro_reset_session();
    }
    switch2_flight_record(Switch2FlightEvent::BOOT, our_descriptor_number, is_switch2_pro_mode(), switch2_pro_ble_enabled, 0);
    usb_init();
    scan_init();
    parse_our_descriptor();
    set_mapping_from_config();

    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));

    struct report_type incoming_report;
    struct descriptor_type incoming_descriptor;
    struct disconnected_type disconnected_item;
    static struct set_report_type set_report_item;
    static uint8_t get_report_tmp_buf[64];
    bool process_pending = false;
    bool get_report_response_pending = false;

    while (true) {
        if (!process_pending) {
            uint8_t reports_to_drain = is_switch_pro_mode() ? 6 : 1;
            while (reports_to_drain-- && !k_msgq_get(&report_q, &incoming_report, K_NO_WAIT)) {
                if (incoming_report.interface == OUR_OUT_INTERFACE && switch_pro_handle_output_report(incoming_report.data, incoming_report.len)) {
                    // Switch Pro output reports are host commands, not remapper inputs.
                } else {
                    handle_received_report(incoming_report.data, incoming_report.len, (uint16_t) incoming_report.interface);
                }
                process_pending = true;
            }
        }
        if (atomic_test_and_clear_bit(tick_pending, 0)) {
            process_mapping(true);
            process_pending = false;
            switch_pro_log_stats();
            switch2_flight_persist();
        }
        if (!k_sem_take(&usb_sem0, K_NO_WAIT)) {
            if (!send_report(do_send_report) && !switch_pro_send_input_heartbeat()) {
                k_sem_give(&usb_sem0);
            }
        }
#if CONFIG_USB_HID_DEVICE_COUNT == 1
        if (!k_sem_take(&switch2_vendor_in_sem, K_NO_WAIT)) {
            if (!switch_pro_send_response()) {
                k_sem_give(&switch2_vendor_in_sem);
            }
        }
#else
        if (!k_sem_take(&usb_sem0, K_NO_WAIT)) {
            if (!switch_pro_send_response()) {
                k_sem_give(&usb_sem0);
            }
        }
#endif
#if CONFIG_USB_HID_DEVICE_COUNT > 1
        if (!k_sem_take(&usb_sem1, K_NO_WAIT)) {
            if (!send_monitor_report(do_send_report)) {
                k_sem_give(&usb_sem1);
            }
        }
#endif

        if (!k_msgq_get(&set_report_q, &set_report_item, K_NO_WAIT)) {
            if (set_report_item.interface == 0) {
                uint8_t switch_report[65];
                switch_report[0] = set_report_item.report_id;
                memcpy(switch_report + 1, set_report_item.data, set_report_item.len);
                if (is_switch_pro_mode()) {
                    switch_pro_set_reports++;
                }
                if (!switch_pro_handle_output_report(switch_report, set_report_item.len + 1)) {
                    handle_set_report0(set_report_item.report_id, set_report_item.data, set_report_item.len);
                }
            }
            if (set_report_item.interface == 1) {
                handle_set_report1(set_report_item.report_id, set_report_item.data, set_report_item.len);
                get_report_response_pending = true;
            }
        }
        if (get_report_response_pending) {
            get_report_response_pending = false;
            uint16_t ret = handle_get_report1(REPORT_ID_CONFIG, get_report_tmp_buf, sizeof(get_report_tmp_buf));
            if (ret > 0) {
                k_mutex_lock(&get_report_mutex, K_FOREVER);
                get_report_response_ready = true;
                memcpy(get_report_buf, get_report_tmp_buf, sizeof(get_report_buf));
                k_mutex_unlock(&get_report_mutex);
            }
        }

        while (!k_msgq_get(&disconnected_q, &disconnected_item, K_NO_WAIT)) {
            LOG_INF("device_disconnected_callback conn_idx=%d", disconnected_item.conn_idx);
            device_disconnected_callback(disconnected_item.conn_idx);
        }

        while (!k_msgq_get(&descriptor_q, &incoming_descriptor, K_NO_WAIT)) {
            LOG_HEXDUMP_DBG(incoming_descriptor.data, incoming_descriptor.size, "incoming_descriptor");
            parse_descriptor(1, 1, incoming_descriptor.data, incoming_descriptor.size, incoming_descriptor.conn_idx << 8, 0);
        }

        if (resume_pending) {
            resume_pending = false;
            suspended = false;
        }
        if (config_updated) {
            set_mapping_from_config();
            config_updated = false;
        }

        if (their_descriptor_updated) {
            update_their_descriptor_derivates();
            their_descriptor_updated = false;
        }

        if (need_to_persist_config) {
            int64_t t0 = k_uptime_get();
            persist_config_return_code = persist_config();
            LOG_INF("persist_config took %lld ms\n", k_uptime_get() - t0);
            need_to_persist_config = false;
            get_report_response_pending = true;
        }

        // without this sleep, some devices won't pair; some thread priority issue?
        k_sleep(K_USEC(1));  // XXX
    }

    return 0;
}
