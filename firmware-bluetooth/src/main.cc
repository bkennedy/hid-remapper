#include <errno.h>
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
#include "switch2_rumble.h"

LOG_MODULE_REGISTER(remapper, LOG_LEVEL_INF);

#define CHK(X) ({ int err = X; if (err != 0) { LOG_ERR("%s returned %d (%s:%d)", #X, err, __FILE__, __LINE__); } err == 0; })

static const int SCAN_DELAY_MS = 1000;
static const int CLEAR_BONDS_BUTTON_PRESS_MS = 3000;
static const uint32_t SWITCH2_CAPTURE_MAGIC = 0x53325248;
static const uint8_t SWITCH2_CAPTURE_VERSION = 6;
static const uint8_t SWITCH2_CAPTURE_ENTRIES = 80;
static const uint8_t SWITCH2_CAPTURE_BYTES = 32;

// these macros don't work in C++ when used directly ("taking address of temporary array")
static auto const BT_UUID_HIDS_ = (struct bt_uuid_16) BT_UUID_INIT_16(BT_UUID_HIDS_VAL);
static auto BT_ADDR_LE_ANY_ = BT_ADDR_LE_ANY[0];
static auto BT_CONN_LE_CREATE_CONN_ = BT_CONN_LE_CREATE_CONN[0];

static struct bt_hogp hogps[CONFIG_BT_MAX_CONN];

static K_SEM_DEFINE(usb_sem0, 1, 1);
#if CONFIG_USB_HID_DEVICE_COUNT > 1
static K_SEM_DEFINE(usb_sem1, 1, 1);
#endif

static struct k_mutex mutexes[(uint8_t) MutexId::N];

static struct k_mutex get_report_mutex;
static uint8_t get_report_buf[64];
static bool get_report_response_ready = false;

static const struct device* hid_dev0;
#if CONFIG_USB_HID_DEVICE_COUNT > 1
static const struct device* hid_dev1;  // config interface
#endif

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

struct switch2_capture_entry {
    uint32_t uptime_ms;
    uint8_t source;
    uint8_t interface;
    uint8_t report_id;
    uint8_t len;
    uint8_t decoded;
    uint8_t low_frequency;
    uint8_t high_frequency;
    uint8_t data[SWITCH2_CAPTURE_BYTES];
};

struct switch2_capture_type {
    uint32_t magic;
    uint8_t version;
    uint8_t write_index;
    uint8_t count;
    uint8_t reserved;
    struct switch2_capture_entry entries[SWITCH2_CAPTURE_ENTRIES];
};

static struct switch2_capture_type switch2_capture;
static bool switch2_capture_loaded = false;
static bool switch2_capture_dirty = false;

K_MSGQ_DEFINE(report_q, sizeof(struct report_type), 16, 4);
K_MSGQ_DEFINE(descriptor_q, sizeof(struct descriptor_type), 2, 4);
K_MSGQ_DEFINE(hogp_ready_q, sizeof(struct hogp_ready_type), CONFIG_BT_MAX_CONN, 4);
K_MSGQ_DEFINE(disconnected_q, sizeof(struct disconnected_type), CONFIG_BT_MAX_CONN, 4);
K_MSGQ_DEFINE(set_report_q, sizeof(struct set_report_type), 8, 4);
K_MSGQ_DEFINE(switch_pro_response_q, 64, 4, 4);
K_MSGQ_DEFINE(switch_pro_test_input_q, 64, 8, 4);
ATOMIC_DEFINE(tick_pending, 1);

static bool switch_pro_input_enabled = false;
static uint8_t switch_pro_input_enable_count = 0;

static void switch_pro_reset_session(bool enable_input);

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
        LOG_DBG("Scanning started.");
        scanning = true;
    }
}

static void scan_stop() {
    if (CHK(bt_scan_stop())) {
        LOG_DBG("Scanning stopped.");
        scanning = false;
        set_led_mode(LedMode::BLINK);
    }
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
    char addr[BT_ADDR_LE_STR_LEN];

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
    count_connections();
    set_led_mode(LedMode::BLINK);

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (conn_err) {
        LOG_ERR("Failed to connect to %s (conn_err=%u).", addr, conn_err);
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));

        return;
    }

    LOG_INF("%s", addr);

    CHK(bt_conn_set_security(conn, BT_SECURITY_L2));
}

static void disconnected(struct bt_conn* conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("%s (reason=%u)", addr, reason);

    uint8_t conn_idx = bt_conn_index(conn);

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
        peers_only = true;
        gatt_discover(conn);
    } else {
        LOG_ERR("security failed: %s, level=%u, err=%d", addr, level, err);
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

    memcpy(buf.data + 1, data, buf.len - 1);
    LOG_DBG("BLE IN conn_idx=%d report_id=%u size=%u first=%02x %02x %02x %02x %02x %02x %02x %02x",
            hogp_index(hogp),
            bt_hogp_rep_id(rep),
            bt_hogp_rep_size(rep),
            buf.len > 1 ? buf.data[1] : 0,
            buf.len > 2 ? buf.data[2] : 0,
            buf.len > 3 ? buf.data[3] : 0,
            buf.len > 4 ? buf.data[4] : 0,
            buf.len > 5 ? buf.data[5] : 0,
            buf.len > 6 ? buf.data[6] : 0,
            buf.len > 7 ? buf.data[7] : 0,
            buf.len > 8 ? buf.data[8] : 0);
    if (k_msgq_put(&report_q, &buf, K_NO_WAIT)) {
        //        printk("error in k_msg_put(report_q\n");
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

static void send_xbox_rumble_stop(struct bt_hogp* hogp);

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

        struct find_bond_t find_bond = {
            .i = 0,
            .found_idx = 0,
        };
        bt_addr_le_copy(&find_bond.addr, bt_conn_get_dst(bt_hogp_conn(item.hogp)));
        bt_foreach_bond(BT_ID_DEFAULT, find_bond_cb, &find_bond);
        LOG_DBG("found bond idx: %d", find_bond.found_idx);
        device_connected_callback(bt_conn_index(bt_hogp_conn(item.hogp)) << 8, 1, 1, find_bond.found_idx);

        while (NULL != (rep = bt_hogp_rep_next(item.hogp, rep))) {
            LOG_INF("HOGP report conn_idx=%d type=%d id=%u size=%u",
                    bt_conn_index(bt_hogp_conn(item.hogp)),
                    bt_hogp_rep_type(rep),
                    bt_hogp_rep_id(rep),
                    bt_hogp_rep_size(rep));
            if (bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_INPUT) {
                LOG_DBG("subscribing to report ID: %u", bt_hogp_rep_id(rep));
                CHK(bt_hogp_rep_subscribe(item.hogp, rep, hogp_notify_cb));
            }
        }

        bt_hogp_map_read(item.hogp, hogp_map_read_cb, 0, K_NO_WAIT);
        send_xbox_rumble_stop(item.hogp);
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
    if (bonded) {
        peers_only = true;
        CHK(settings_save());
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
    }
}

static void pairing_failed(struct bt_conn* conn, enum bt_security_err reason) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_ERR("%s, reason %d", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed
};

static const char* switch2_capture_source_name(uint8_t source) {
    switch (source) {
        case 1: return "control_raw";
        case 2: return "control_sync";
        case 3: return "control_queued";
        case 4: return "interrupt_raw";
        case 5: return "interrupt_queued";
        case 6: return "get_report";
        case 7: return "usb_configured";
        case 8: return "input_report";
        default: return "unknown";
    }
}

static void switch2_capture_init_empty() {
    memset(&switch2_capture, 0, sizeof(switch2_capture));
    switch2_capture.magic = SWITCH2_CAPTURE_MAGIC;
    switch2_capture.version = SWITCH2_CAPTURE_VERSION;
}

static void switch2_capture_persist_work_fn(struct k_work* work) {
    ARG_UNUSED(work);

    if (switch2_capture_dirty) {
        if (CHK(settings_save_one("remapper/switch2_log", &switch2_capture, sizeof(switch2_capture)))) {
            switch2_capture_dirty = false;
        }
    }
}

static K_WORK_DELAYABLE_DEFINE(switch2_capture_persist_work, switch2_capture_persist_work_fn);

static void switch2_capture_record(uint8_t source, uint8_t interface, uint8_t report_id, const uint8_t* data, uint16_t len) {
    if ((source == 1) || (source == 4)) {
        return;
    }

    if (!switch2_capture_loaded ||
        switch2_capture.magic != SWITCH2_CAPTURE_MAGIC ||
        switch2_capture.version != SWITCH2_CAPTURE_VERSION) {
        switch2_capture_init_empty();
        switch2_capture_loaded = true;
    }

    struct switch2_capture_entry* entry = &switch2_capture.entries[switch2_capture.write_index];
    uint16_t copy_len = len < SWITCH2_CAPTURE_BYTES ? len : SWITCH2_CAPTURE_BYTES;

    memset(entry, 0, sizeof(*entry));
    entry->uptime_ms = k_uptime_get_32();
    entry->source = source;
    entry->interface = interface;
    entry->report_id = report_id;
    entry->len = len > 255 ? 255 : len;
    memcpy(entry->data, data, copy_len);
    entry->decoded = switch2_rumble_decode((len > 0 && data[0] == report_id) ? 0 : report_id, data, len, &entry->low_frequency, &entry->high_frequency) ? 1 : 0;

    switch2_capture.write_index = (switch2_capture.write_index + 1) % SWITCH2_CAPTURE_ENTRIES;
    if (switch2_capture.count < SWITCH2_CAPTURE_ENTRIES) {
        switch2_capture.count++;
    }
}

static void log_host_output_report(uint8_t source, uint8_t interface, uint8_t report_id, const uint8_t* data, uint16_t len) {
    switch2_capture_record(source, interface, report_id, data, len);
}

static int set_report_cb(const struct device* dev, struct usb_setup_packet* setup, int32_t* len, uint8_t** data) {
    uint8_t request_value[2];

    // report_id, report_type
    sys_put_le16(setup->wValue, request_value);

    struct set_report_type buf;
    if ((request_value[0] > 0) && (*len > 0)) {
        uint8_t interface = 0xff;
        if (dev == hid_dev0) {
            interface = 0;
        }
#if CONFIG_USB_HID_DEVICE_COUNT > 1
        else if (dev == hid_dev1) {
            interface = 1;
        }
#endif
        if (dev == hid_dev0) {
            log_host_output_report(1, interface, request_value[0], *data, *len);
        }

        bool data_includes_report_id = ((*len > 0) && ((*data)[0] == request_value[0]));
        uint8_t* payload = data_includes_report_id ? (*data) + 1 : *data;
        uint16_t payload_len = data_includes_report_id ? (*len) - 1 : *len;

        if ((dev == hid_dev0) && set_report0_synchronous(request_value[0])) {
            log_host_output_report(2, 0, request_value[0], payload, payload_len);
            handle_set_report0(request_value[0], payload, payload_len);
        } else {
            if (dev == hid_dev0) {
                buf.interface = 0;
            }
#if CONFIG_USB_HID_DEVICE_COUNT > 1
            else if (dev == hid_dev1) {
                buf.interface = 1;
                k_mutex_lock(&get_report_mutex, K_FOREVER);
                get_report_response_ready = false;
                k_mutex_unlock(&get_report_mutex);
            }
#endif
            buf.report_id = request_value[0];
            buf.len = payload_len;
            memcpy(buf.data, payload, payload_len);
            if (dev == hid_dev0) {
                log_host_output_report(3, buf.interface, buf.report_id, buf.data, buf.len);
            }
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

    if (dev == hid_dev0) {
        uint8_t request_data[] = {
            request_value[1],
            (uint8_t)(setup->wLength & 0xff),
            (uint8_t)(setup->wLength >> 8),
            (uint8_t)(*len > 255 ? 255 : *len),
        };
        log_host_output_report(6, 0, request_value[0], request_data, sizeof(request_data));
        LOG_INF("SWITCH2 GET_REPORT iface=0 report_id=%u report_type=%u wLength=%u current_len=%d",
                request_value[0],
                request_value[1],
                setup->wLength,
                *len);
    }
#if CONFIG_USB_HID_DEVICE_COUNT > 1
    else {
        LOG_DBG("report_id=%d, %d, len=%d", request_value[0], request_value[1], *len);
    }
#endif

    *data[0] = request_value[0];
    if (dev == hid_dev0) {
        *len = handle_get_report0(request_value[0], (*data) + 1, CONFIG_SIZE);
    }
#if CONFIG_USB_HID_DEVICE_COUNT > 1
    else if (dev == hid_dev1) {
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
    }
#endif
    (*len)++;

    return 0;
};

static void int_in_ready_cb0(const struct device* dev) {
    k_sem_give(&usb_sem0);
}

static void int_out_ready_cb0(const struct device* dev) {
    static struct set_report_type buf;
    uint32_t len;
    if (CHK(hid_int_ep_read(hid_dev0, buf.data, sizeof(buf.data), &len))) {
        log_host_output_report(4, 0, len > 0 ? buf.data[0] : 0, buf.data, len);
        if (len == 0) {
            return;
        }

        buf.interface = 0;
        buf.report_id = buf.data[0];
        buf.len = len - 1;
        memmove(buf.data, buf.data + 1, buf.len);
        log_host_output_report(5, 0, buf.report_id, buf.data, buf.len);
        CHK(k_msgq_put(&set_report_q, &buf, K_NO_WAIT));
    }
}

#if CONFIG_USB_HID_DEVICE_COUNT > 1
static void int_in_ready_cb1(const struct device* dev) {
    k_sem_give(&usb_sem1);
}
#endif

static const struct hid_ops ops0 = {
    .get_report = get_report_cb,
    .set_report = set_report_cb,
    .int_in_ready = int_in_ready_cb0,
    .int_out_ready = int_out_ready_cb0,
};

#if CONFIG_USB_HID_DEVICE_COUNT > 1
static const struct hid_ops ops1 = {
    .get_report = get_report_cb,
    .set_report = set_report_cb,
    .int_in_ready = int_in_ready_cb1,
};
#endif

static uint8_t switch_pro_report_timer = 0;
static int64_t switch_pro_last_heartbeat_ms = 0;
static uint32_t switch_pro_heartbeat_count = 0;
static uint8_t switch_pro_current_input_report[64];
static bool switch_pro_current_input_valid = false;

static void switch_pro_reset_current_input_report() {
    memset(switch_pro_current_input_report, 0, sizeof(switch_pro_current_input_report));
    switch_pro_current_input_report[0] = 0x30;
    switch_pro_current_input_report[2] = 0x81;
    switch_pro_current_input_report[4] = 0x80;
    switch_pro_current_input_report[7] = 0x08;
    switch_pro_current_input_report[8] = 0x80;
    switch_pro_current_input_report[10] = 0x08;
    switch_pro_current_input_report[11] = 0x80;
    switch_pro_current_input_report[12] = 0x09;
    switch_pro_current_input_valid = true;
}

static uint8_t switch_pro_next_input_timer() {
    switch_pro_report_timer += 2;
    return switch_pro_report_timer;
}

static uint8_t switch_pro_next_response_timer() {
    switch_pro_report_timer += 3;
    return switch_pro_report_timer;
}

static void switch_pro_reset_session(bool enable_input) {
    switch_pro_report_timer = 0;
    switch_pro_last_heartbeat_ms = 0;
    switch_pro_heartbeat_count = 0;
    switch_pro_reset_current_input_report();
    switch_pro_input_enable_count = 0;
    switch_pro_input_enabled = enable_input && (our_descriptor_number == 2);
    k_msgq_purge(&switch_pro_response_q);
    k_msgq_purge(&switch_pro_test_input_q);
    LOG_INF("SWITCH2 Pro session reset input_enabled=%u", switch_pro_input_enabled ? 1 : 0);
}

static void switch_pro_prepare_input_report(uint8_t* report_with_id, uint8_t len) {
    if ((len < 13) || (report_with_id[0] != 0x30)) {
        return;
    }

    uint8_t timer = switch_pro_next_input_timer();
    report_with_id[1] = timer;
    report_with_id[12] = 0x09 + (timer & 0x03);
}

static bool do_send_report(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    uint8_t patched_report[64];

    if ((our_descriptor_number == 2) &&
        (interface == 0) &&
        (len > 0) &&
        (report_with_id[0] == 0x30) &&
        !switch_pro_input_enabled) {
        return true;
    }

    if ((interface == 0) && (len <= sizeof(patched_report)) && (len >= 13) && (report_with_id[0] == 0x30)) {
        memcpy(patched_report, report_with_id, len);
        switch_pro_prepare_input_report(patched_report, len);
        report_with_id = patched_report;
        memcpy(switch_pro_current_input_report, report_with_id, len);
        switch_pro_current_input_valid = true;
    }

    if ((interface == 0) && (len >= 12) && (report_with_id[0] == 0x30)) {
        static uint8_t last_switch_pro_input[10];
        static bool last_switch_pro_input_valid = false;
        if (!last_switch_pro_input_valid || memcmp(last_switch_pro_input, report_with_id + 2, sizeof(last_switch_pro_input))) {
            memcpy(last_switch_pro_input, report_with_id + 2, sizeof(last_switch_pro_input));
            last_switch_pro_input_valid = true;
            LOG_INF("SWITCH2 IN report_id=30 len=%u first=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                    len,
                    report_with_id[1],
                    report_with_id[2],
                    report_with_id[3],
                    report_with_id[4],
                    report_with_id[5],
                    report_with_id[6],
                    report_with_id[7],
                    report_with_id[8],
                    report_with_id[9],
                    report_with_id[10],
                    report_with_id[11]);
        }
    }

    if (report_with_id[0] == 0) {
        report_with_id++;
        len--;
    }
    if (interface == 0) {
        return CHK(hid_int_ep_write(hid_dev0, report_with_id, len, NULL));
    }
#if CONFIG_USB_HID_DEVICE_COUNT > 1
    if (interface == 1) {
        return CHK(hid_int_ep_write(hid_dev1, report_with_id, len, NULL));
    }
#endif
    return false;
}

static void switch_pro_queue_test_input();

static void switch_pro_fill_input_common(uint8_t* report, uint8_t report_id, uint8_t timer) {
    memset(report, 0, 64);
    report[0] = report_id;
    report[1] = timer;
    report[2] = 0x81;
    report[4] = 0x80;
    report[6] = 0x00;
    report[7] = 0x08;
    report[8] = 0x80;
    report[9] = 0x00;
    report[10] = 0x08;
    report[11] = 0x80;
    report[12] = 0x00;
}

static void switch_pro_queue_response_81(uint8_t command, const uint8_t* data, uint8_t len) {
    uint8_t response[64] = {};

    response[0] = 0x81;
    response[1] = command;
    if (len > sizeof(response) - 2) {
        len = sizeof(response) - 2;
    }
    if (data != NULL && len > 0) {
        memcpy(response + 2, data, len);
    }

    CHK(k_msgq_put(&switch_pro_response_q, response, K_NO_WAIT));
}

static void switch_pro_handle_usb_init_output(uint8_t report_id, const uint8_t* data, uint16_t len) {
    if ((our_descriptor_number != 2) || (report_id != 0x80) || (len == 0)) {
        return;
    }

    static const uint8_t mac_response[] = {
        0x00, 0x03, 0x1f, 0x86, 0x1d, 0xd6, 0x03, 0x04,
    };

    LOG_INF("SWITCH2 Pro USB HID cmd=%02x len=%u arg=%02x %02x %02x %02x",
            data[0],
            len,
            len > 1 ? data[1] : 0,
            len > 2 ? data[2] : 0,
            len > 3 ? data[3] : 0,
            len > 4 ? data[4] : 0);

    switch (data[0]) {
        case 0x01:
            LOG_INF("SWITCH2 Pro init: queue MAC response");
            switch_pro_queue_response_81(data[0], mac_response, sizeof(mac_response));
            break;
        case 0x02:
            LOG_INF("SWITCH2 Pro init: queue handshake response");
            switch_pro_queue_response_81(data[0], NULL, 0);
            break;
        case 0x03:
            LOG_INF("SWITCH2 Pro init: queue baud response");
            switch_pro_queue_response_81(data[0], NULL, 0);
            break;
        case 0x04:
            LOG_INF("SWITCH2 Pro init: USB HID joystick enabled");
            break;
        case 0x05:
            LOG_INF("SWITCH2 Pro init: USB HID joystick disabled");
            switch_pro_input_enabled = false;
            break;
        case 0x06:
            LOG_INF("SWITCH2 Pro init: queue reset response");
            switch_pro_reset_session(false);
            switch_pro_queue_response_81(data[0], NULL, 0);
            break;
        case 0x91:
        case 0x92:
            LOG_INF("SWITCH2 Pro init: unsupported UART bridge cmd=%02x", data[0]);
            switch_pro_queue_response_81(data[0], NULL, 0);
            break;
        default:
            LOG_INF("SWITCH2 Pro init: unknown USB HID cmd=%02x", data[0]);
            switch_pro_queue_response_81(data[0], NULL, 0);
            break;
    }
}

static void switch_pro_queue_subcommand_response(uint8_t subcommand, const uint8_t* data, uint8_t len) {
    uint8_t response[64];

    switch_pro_fill_input_common(response, 0x21, 0);
    response[13] = subcommand == 0x02 ? 0x82 : 0x80;
    response[14] = subcommand;

    if (len > sizeof(response) - 15) {
        len = sizeof(response) - 15;
    }
    if ((data != NULL) && (len > 0)) {
        memcpy(response + 15, data, len);
    }

    CHK(k_msgq_put(&switch_pro_test_input_q, response, K_NO_WAIT));
}

static void switch_pro_pack_u12_pair(uint8_t* out, uint16_t a, uint16_t b) {
    out[0] = a & 0xff;
    out[1] = ((a >> 8) & 0x0f) | ((b & 0x0f) << 4);
    out[2] = (b >> 4) & 0xff;
}

static void switch_pro_copy_from_region(uint16_t address,
                                        uint16_t region_start,
                                        const uint8_t* region,
                                        uint16_t region_len,
                                        uint8_t* data,
                                        uint8_t len) {
    uint16_t offset = address - region_start;
    if (offset >= region_len) {
        return;
    }
    uint16_t copy_len = region_len - offset;
    if (copy_len > len) {
        copy_len = len;
    }
    memcpy(data, region + offset, copy_len);
}

static bool switch_pro_spi_read_data(uint16_t address, uint8_t* data, uint8_t len) {
    memset(data, 0xff, len);

    if ((address >= 0x5000) && (address + len <= 0x6000)) {
        if (address == 0x5000 && len > 0) {
            data[0] = 0x50;
        }
        return true;
    }

    static const uint8_t factory_6000[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0x03, 0x01, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0x01, 0xff, 0xff, 0xff, 0xff,
    };

    if ((address >= 0x6000) && (address < 0x6020)) {
        switch_pro_copy_from_region(address, 0x6000, factory_6000, sizeof(factory_6000), data, len);
        return true;
    }

    if ((address == 0x603d) && (len >= 18)) {
        static const uint8_t mayflash_stick_calibration[] = {
            0xb2, 0xe5, 0x62, 0x89, 0xc7, 0x7c,
            0xe5, 0xa5, 0x5c, 0x67, 0x77, 0x71,
            0x9c, 0x65, 0x5a, 0x3b, 0xe6, 0x61,
        };
        memcpy(data, mayflash_stick_calibration, sizeof(mayflash_stick_calibration));
        return true;
    }

    if ((address == 0x6086) && (len >= 18)) {
        static const uint8_t mayflash_stick_parameters[] = {
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0x83, 0xff,
        };
        memcpy(data, mayflash_stick_parameters, sizeof(mayflash_stick_parameters));
        return true;
    }

    if ((address == 0x6020) && (len >= 24)) {
        static const uint8_t mayflash_factory_calibration[] = {
            0x83, 0xff, 0x10, 0xff, 0xd7, 0x02,
            0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
            0x09, 0x00, 0xca, 0xff, 0xc8, 0xff,
            0x3b, 0x34, 0x3b, 0x34, 0x3b, 0x34,
        };
        memcpy(data, mayflash_factory_calibration, sizeof(mayflash_factory_calibration));
        return true;
    }

    if ((address == 0x6050) && (len >= 6)) {
        static const uint8_t gray_colors[] = {
            0x32, 0x32, 0x32,
            0xff, 0xff, 0xff,
            0x82, 0x82, 0x82,
            0x0f, 0x0f, 0x0f,
            0x01,
        };
        switch_pro_copy_from_region(address, 0x6050, gray_colors, sizeof(gray_colors), data, len);
        return true;
    }

    if ((address == 0x6080) && (len >= 6)) {
        static const uint8_t model1[] = {
            0x50, 0xfd, 0x00, 0x00, 0xc6, 0x0f,
            0x0c, 0x80, 0x00, 0x08, 0x80, 0x00,
            0x0c, 0x80, 0x00, 0x08, 0x80, 0x00,
            0x0c, 0x80, 0x00, 0x08, 0x80, 0x00,
        };
        switch_pro_copy_from_region(address, 0x6080, model1, sizeof(model1), data, len);
        return true;
    }

    if ((address == 0x6098) && (len >= 18)) {
        static const uint8_t mayflash_model_parameters[] = {
            0x0f, 0x30, 0x61, 0x96, 0x30, 0xf3,
            0xd4, 0x14, 0x54, 0x41, 0x15, 0x54,
            0xc7, 0x79, 0x9c, 0x33, 0x36, 0x63,
        };
        switch_pro_copy_from_region(address, 0x6098, mayflash_model_parameters, sizeof(mayflash_model_parameters), data, len);
        return true;
    }

    if ((address >= 0x8010) && (address < 0x8040)) {
        static const uint8_t mayflash_user_calibration_8010[] = {
            0xb2, 0xa1, 0xff, 0xf7, 0x7f, 0x00,
            0x08, 0x80, 0x00, 0x08, 0x80, 0xb2,
            0xa1, 0x00, 0x08, 0x80, 0x00, 0x08,
            0x80, 0xff, 0xf7, 0x7f,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0x83, 0xff,
            0x10, 0xff, 0xd7, 0x02, 0x00, 0x40,
            0x00, 0x40,
        };
        switch_pro_copy_from_region(address, 0x8010, mayflash_user_calibration_8010, sizeof(mayflash_user_calibration_8010), data, len);
        return true;
    }

    if ((address >= 0x8000) && (address + len <= 0x9000)) {
        return true;
    }

    return false;
}

static void switch_pro_queue_spi_read_response(const uint8_t* data, uint16_t len) {
    if (len < 15) {
        switch_pro_queue_subcommand_response(0x10, NULL, 0);
        return;
    }

    uint8_t response[64];
    uint16_t address = data[10] | (data[11] << 8);
    uint8_t read_len = data[14];

    if (read_len > sizeof(response) - 20) {
        read_len = sizeof(response) - 20;
    }

    switch_pro_fill_input_common(response, 0x21, 0);
    response[13] = 0x90;
    response[14] = 0x10;
    response[15] = address & 0xff;
    response[16] = (address >> 8) & 0xff;
    response[19] = read_len;
    if (!switch_pro_spi_read_data(address, response + 20, read_len)) {
        LOG_INF("SWITCH2 Pro SPI read ignored unknown addr=%04x len=%u", address, read_len);
        return;
    }

    LOG_INF("SWITCH2 Pro SPI read addr=%04x len=%u", address, read_len);
    CHK(k_msgq_put(&switch_pro_test_input_q, response, K_NO_WAIT));
}

static void switch_pro_handle_subcommand_output(uint8_t report_id, const uint8_t* data, uint16_t len) {
    if ((our_descriptor_number != 2) || (report_id != 0x01) || (len < 10)) {
        return;
    }

    uint8_t subcommand = data[9];
    LOG_INF("SWITCH2 Pro subcommand id=%02x len=%u arg=%02x %02x %02x %02x %02x rumble=%02x %02x %02x %02x %02x %02x %02x %02x",
            subcommand,
            len,
            len > 10 ? data[10] : 0,
            len > 11 ? data[11] : 0,
            len > 12 ? data[12] : 0,
            len > 13 ? data[13] : 0,
            len > 14 ? data[14] : 0,
            data[1],
            data[2],
            data[3],
            data[4],
            data[5],
            data[6],
            data[7],
            data[8]);

    switch (subcommand) {
        case 0x02: {
            static const uint8_t device_info[] = {
                0x03, 0x48, 0x03, 0x02,
                0x53, 0x50, 0x00, 0x01, 0x0d, 0x4b,
                0x01, 0x01,
            };
            switch_pro_queue_subcommand_response(subcommand, device_info, sizeof(device_info));
            break;
        }
        case 0x10:
            switch_pro_queue_spi_read_response(data, len);
            break;
        case 0x03:
        case 0x04:
        case 0x30:
        case 0x33:
        case 0x38:
        case 0x40:
        case 0x41:
        case 0x48:
            switch_pro_queue_subcommand_response(subcommand, NULL, 0);
            break;
        default:
            switch_pro_queue_subcommand_response(subcommand, NULL, 0);
            break;
    }

    if ((subcommand == 0x03) && (len > 10) && (data[10] == 0x30)) {
        if (switch_pro_input_enable_count < 0xff) {
            switch_pro_input_enable_count++;
        }
        switch_pro_input_enabled = true;
        LOG_INF("SWITCH2 Pro input heartbeat enable count=%u enabled=%u",
                switch_pro_input_enable_count,
                switch_pro_input_enabled ? 1 : 0);
    }
}

static bool send_switch_pro_response() {
    uint8_t response[64];

    if (k_msgq_get(&switch_pro_response_q, response, K_NO_WAIT)) {
        return false;
    }

    LOG_INF("SWITCH2 Pro response report_id=%02x cmd=%02x", response[0], response[1]);
    return do_send_report(0, response, sizeof(response));
}

static void switch_pro_make_test_input(uint8_t* report, uint8_t timer, bool press_a) {
    switch_pro_fill_input_common(report, 0x30, timer);
    report[3] = press_a ? 0x08 : 0x00;
}

static void switch_pro_queue_test_input() {
    uint8_t report[64];

    LOG_INF("SWITCH2 TEST queue A press");
    switch_pro_make_test_input(report, 0x00, false);
    CHK(k_msgq_put(&switch_pro_test_input_q, report, K_NO_WAIT));
    switch_pro_make_test_input(report, 0x03, true);
    CHK(k_msgq_put(&switch_pro_test_input_q, report, K_NO_WAIT));
    switch_pro_make_test_input(report, 0x06, true);
    CHK(k_msgq_put(&switch_pro_test_input_q, report, K_NO_WAIT));
    switch_pro_make_test_input(report, 0x09, false);
    CHK(k_msgq_put(&switch_pro_test_input_q, report, K_NO_WAIT));
}

static bool send_switch_pro_test_input() {
    uint8_t report[64];

    if (k_msgq_get(&switch_pro_test_input_q, report, K_NO_WAIT)) {
        return false;
    }

    if (report[0] == 0x21) {
        report[1] = switch_pro_next_response_timer();
    }

    LOG_INF("SWITCH2 TEST send report_id=%02x timer=%u buttons=%02x %02x %02x ack=%02x subcmd=%02x",
            report[0],
            report[1],
            report[3],
            report[4],
            report[5],
            report[13],
            report[14]);
    return do_send_report(0, report, sizeof(report));
}

static bool send_switch_pro_input_heartbeat() {
    if ((our_descriptor_number != 2) || !switch_pro_input_enabled) {
        return false;
    }

    int64_t now_ms = k_uptime_get();
    if (now_ms - switch_pro_last_heartbeat_ms < 8) {
        return false;
    }
    switch_pro_last_heartbeat_ms = now_ms;

    if (!switch_pro_current_input_valid) {
        switch_pro_reset_current_input_report();
    }

    switch_pro_heartbeat_count++;
    if ((switch_pro_heartbeat_count & 0x7f) == 0) {
        LOG_INF("SWITCH2 Pro heartbeat count=%u state=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                switch_pro_heartbeat_count,
                switch_pro_current_input_report[2],
                switch_pro_current_input_report[3],
                switch_pro_current_input_report[4],
                switch_pro_current_input_report[5],
                switch_pro_current_input_report[6],
                switch_pro_current_input_report[7],
                switch_pro_current_input_report[8],
                switch_pro_current_input_report[9],
                switch_pro_current_input_report[10],
                switch_pro_current_input_report[11]);
    }
    return do_send_report(0, switch_pro_current_input_report, sizeof(switch_pro_current_input_report));
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
    } else if (status == USB_DC_CONFIGURED) {
        suspended = false;
        switch_pro_reset_session(false);
    } else if ((status == USB_DC_RESET) ||
               (status == USB_DC_DISCONNECTED) ||
               (status == USB_DC_SUSPEND)) {
        suspended = true;
        switch_pro_reset_session(false);
    }
}

extern struct usb_device_descriptor __usb_descriptor_start[];

static void descriptor_init() {
    our_descriptor = &our_descriptors[our_descriptor_number];
    if ((our_descriptor->vid != 0) && (our_descriptor->pid != 0)) {
        struct usb_device_descriptor* device_descriptor = __usb_descriptor_start;
        device_descriptor->idVendor = our_descriptor->vid;
        device_descriptor->idProduct = our_descriptor->pid;
        if (our_descriptor_number == 2) {
            device_descriptor->bcdDevice = 0x0200;
        }
    }
}

static void usb_init() {
    suspended = true;

    hid_dev0 = device_get_binding("HID_0");
    if (hid_dev0 == NULL) {
        LOG_ERR("Cannot get USB HID Device 0.");
        return;
    }

    usb_hid_register_device(hid_dev0, our_descriptor->descriptor, our_descriptor->descriptor_length, &ops0);
    CHK(usb_hid_init(hid_dev0));
#if CONFIG_USB_HID_DEVICE_COUNT > 1
    hid_dev1 = device_get_binding("HID_1");
    if (hid_dev1 == NULL) {
        LOG_ERR("Cannot get USB HID Device 1.");
        return;
    }
    usb_hid_register_device(hid_dev1, config_report_descriptor, config_report_descriptor_length, &ops1);
    CHK(usb_hid_init(hid_dev1));
#endif
    CHK(usb_enable(status_cb));
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

static void switch2_capture_dump_and_clear_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(switch2_capture_dump_work, switch2_capture_dump_and_clear_work_fn);

static void switch2_capture_dump_and_clear_work_fn(struct k_work* work) {
    ARG_UNUSED(work);

    if (!switch2_capture_loaded ||
        switch2_capture.magic != SWITCH2_CAPTURE_MAGIC ||
        switch2_capture.version != SWITCH2_CAPTURE_VERSION ||
        switch2_capture.count == 0) {
        LOG_INF("SWITCH2 CAPTURE empty");
        return;
    }

    LOG_INF("SWITCH2 CAPTURE dump count=%u next=%u", switch2_capture.count, switch2_capture.write_index);
    for (uint8_t n = 0; n < switch2_capture.count; n++) {
        uint8_t index = (switch2_capture.write_index + SWITCH2_CAPTURE_ENTRIES - switch2_capture.count + n) % SWITCH2_CAPTURE_ENTRIES;
        struct switch2_capture_entry* entry = &switch2_capture.entries[index];
        uint8_t dump_len = entry->len < SWITCH2_CAPTURE_BYTES ? entry->len : SWITCH2_CAPTURE_BYTES;
        const uint8_t* data = entry->data;
        LOG_INF("SWITCH2 CAPTURE %u source=%s iface=%u report_id=%u len=%u decoded=%u low=%u high=%u t=%u data=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                n,
                switch2_capture_source_name(entry->source),
                entry->interface,
                entry->report_id,
                entry->len,
                entry->decoded,
                entry->low_frequency,
                entry->high_frequency,
                entry->uptime_ms,
                0 < dump_len ? data[0] : 0,
                1 < dump_len ? data[1] : 0,
                2 < dump_len ? data[2] : 0,
                3 < dump_len ? data[3] : 0,
                4 < dump_len ? data[4] : 0,
                5 < dump_len ? data[5] : 0,
                6 < dump_len ? data[6] : 0,
                7 < dump_len ? data[7] : 0,
                8 < dump_len ? data[8] : 0,
                9 < dump_len ? data[9] : 0,
                10 < dump_len ? data[10] : 0,
                11 < dump_len ? data[11] : 0,
                12 < dump_len ? data[12] : 0,
                13 < dump_len ? data[13] : 0,
                14 < dump_len ? data[14] : 0,
                15 < dump_len ? data[15] : 0);
    }

    switch2_capture_init_empty();
    switch2_capture_dirty = false;
    CHK(settings_save_one("remapper/switch2_log", &switch2_capture, sizeof(switch2_capture)));
}

static int remapper_settings_set(const char* name, size_t len, settings_read_cb read_cb, void* cb_arg) {
    LOG_INF("len=%d", len);

    static uint8_t buffer[PERSISTED_CONFIG_SIZE];

    if (!strcmp(name, "switch2_log")) {
        if (len != sizeof(switch2_capture)) {
            switch2_capture_init_empty();
            switch2_capture_loaded = true;
            return -EINVAL;
        }

        int bytes_read = read_cb(cb_arg, &switch2_capture, len);
        if (bytes_read < 0) {
            return bytes_read;
        }
        if (bytes_read != sizeof(switch2_capture) ||
            switch2_capture.magic != SWITCH2_CAPTURE_MAGIC ||
            switch2_capture.version != SWITCH2_CAPTURE_VERSION ||
            switch2_capture.write_index >= SWITCH2_CAPTURE_ENTRIES ||
            switch2_capture.count > SWITCH2_CAPTURE_ENTRIES) {
            switch2_capture_init_empty();
        }
        switch2_capture_loaded = true;
        return 0;
    }

    if (strcmp(name, "config")) {
        return -ENOENT;
    }

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
    CHK(settings_save_one("remapper/config", buffer, PERSISTED_CONFIG_SIZE));
}

// https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/master/src/main.c#L116
const int DFU_MAGIC_UF2_RESET = 0x57;

void reset_to_bootloader() {
    sys_reboot(DFU_MAGIC_UF2_RESET);
}

void flash_b_side() {
}

static void rumble_write_cb(struct bt_hogp* hogp, struct bt_hogp_rep_info* rep, uint8_t err) {
    LOG_INF("RUMBLE write complete conn_idx=%d report_id=%u err=%u",
            hogp_index(hogp),
            bt_hogp_rep_id(rep),
            err);
}

static void send_xbox_rumble_stop(struct bt_hogp* hogp) {
    if (!bt_hogp_ready_check(hogp)) {
        return;
    }

    uint8_t stop_report[] = {
        0x03,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    };

    struct bt_hogp_rep_info* rep = NULL;
    while (NULL != (rep = bt_hogp_rep_next(hogp, rep))) {
        if (bt_hogp_rep_type(rep) != BT_HIDS_REPORT_TYPE_OUTPUT) {
            continue;
        }
        uint8_t rep_id = bt_hogp_rep_id(rep);
        if (rep_id != 0x03 && rep_id != 0) {
            continue;
        }
        int err = bt_hogp_rep_write_wo_rsp(hogp, rep, stop_report, sizeof(stop_report), rumble_write_cb);
        LOG_INF("RUMBLE stop conn_idx=%d report_id=%u err=%d", hogp_index(hogp), rep_id, err);
        break;
    }
}

void host_rumble_received(uint8_t report_id, const uint8_t* buffer, uint16_t len) {
    uint8_t low_frequency = 0;
    uint8_t high_frequency = 0;

    LOG_DBG("RUMBLE host report id=%u len=%u first=%02x %02x %02x %02x",
            report_id,
            len,
            len > 0 ? buffer[0] : 0,
            len > 1 ? buffer[1] : 0,
            len > 2 ? buffer[2] : 0,
            len > 3 ? buffer[3] : 0);

    if (!switch2_rumble_decode(report_id, buffer, len, &low_frequency, &high_frequency)) {
        LOG_DBG("RUMBLE decode skipped");
        return;
    }

    LOG_DBG("RUMBLE forwarding disabled while debugging Switch 2 stability low=%u high=%u",
            low_frequency,
            high_frequency);
    return;

    uint8_t strong_percent = (uint32_t) low_frequency * 100 / 255;
    uint8_t weak_percent = (uint32_t) high_frequency * 100 / 255;

    if (low_frequency || high_frequency) {
        LOG_INF("RUMBLE decoded low=%u high=%u strong_pct=%u weak_pct=%u",
                low_frequency, high_frequency, strong_percent, weak_percent);
    } else {
        LOG_DBG("RUMBLE decoded low=0 high=0");
        return;
    }

    // Xbox Bluetooth HID rumble report 0x03 payload. The Linux hid-microsoft
    // driver sends the report ID followed by this layout; HOGP selects the
    // report ID via the Report Reference characteristic, so it is omitted here.
    uint8_t xbox_rumble[] = {
        0x03,
        0x00,
        0x00,
        strong_percent,
        weak_percent,
        0xff,
        0x00,
        0xff,
    };

    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
        struct bt_hogp* hogp = &hogps[i];
        if (!bt_hogp_assign_check(hogp)) {
            LOG_DBG("RUMBLE conn_idx=%d no HOGP assignment", i);
            continue;
        }

        if (!bt_hogp_ready_check(hogp)) {
            LOG_WRN("RUMBLE conn_idx=%d HOGP not ready", i);
            continue;
        }

        struct bt_hogp_rep_info* rep = NULL;
        bool sent = false;
        while (NULL != (rep = bt_hogp_rep_next(hogp, rep))) {
            LOG_INF("RUMBLE candidate conn_idx=%d type=%d id=%u size=%u",
                    i, bt_hogp_rep_type(rep), bt_hogp_rep_id(rep), bt_hogp_rep_size(rep));
            if (bt_hogp_rep_type(rep) != BT_HIDS_REPORT_TYPE_OUTPUT) {
                continue;
            }

            uint8_t rep_id = bt_hogp_rep_id(rep);
            if (rep_id != 0x03 && rep_id != 0) {
                continue;
            }

            int err = bt_hogp_rep_write_wo_rsp(hogp, rep, xbox_rumble, sizeof(xbox_rumble), rumble_write_cb);
            if (err) {
                LOG_WRN("rumble write failed conn_idx=%d report_id=%u err=%d", i, rep_id, err);
            } else {
                LOG_INF("RUMBLE write ok conn_idx=%d report_id=%u bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
                        i,
                        rep_id,
                        xbox_rumble[0],
                        xbox_rumble[1],
                        xbox_rumble[2],
                        xbox_rumble[3],
                        xbox_rumble[4],
                        xbox_rumble[5],
                        xbox_rumble[6],
                        xbox_rumble[7]);
                sent = true;
            }
            break;
        }

        if (!sent) {
            LOG_DBG("no Xbox rumble output report found for conn_idx=%d", i);
        }
    }
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
    bt_init();
    CHK(settings_subsys_init());
    CHK(settings_register(&our_settings_handlers));
    settings_load();
    our_descriptor_number = 2;
    if (!switch2_capture_loaded) {
        switch2_capture_init_empty();
        switch2_capture_loaded = true;
    }
    descriptor_init();
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
        if (!process_pending && !k_msgq_get(&report_q, &incoming_report, K_NO_WAIT)) {
            handle_received_report(incoming_report.data, incoming_report.len, (uint16_t) incoming_report.interface);
            process_pending = true;
        }
        if (atomic_test_and_clear_bit(tick_pending, 0)) {
            process_mapping(true);
            process_pending = false;
        }
        if (!k_sem_take(&usb_sem0, K_NO_WAIT)) {
            if (!send_switch_pro_response() &&
                !send_switch_pro_test_input() &&
                !send_report(do_send_report) &&
                !send_switch_pro_input_heartbeat()) {
                k_sem_give(&usb_sem0);
            }
        }
#if CONFIG_USB_HID_DEVICE_COUNT > 1
        if (!k_sem_take(&usb_sem1, K_NO_WAIT)) {
            if (!send_monitor_report(do_send_report)) {
                k_sem_give(&usb_sem1);
            }
        }
#endif

        if (!k_msgq_get(&set_report_q, &set_report_item, K_NO_WAIT)) {
            if (set_report_item.interface == 0) {
                switch_pro_handle_usb_init_output(set_report_item.report_id, set_report_item.data, set_report_item.len);
                switch_pro_handle_subcommand_output(set_report_item.report_id, set_report_item.data, set_report_item.len);
                handle_set_report0(set_report_item.report_id, set_report_item.data, set_report_item.len);
            }
            if (set_report_item.interface == 1) {
#if CONFIG_USB_HID_DEVICE_COUNT > 1
                handle_set_report1(set_report_item.report_id, set_report_item.data, set_report_item.len);
                get_report_response_pending = true;
#endif
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
