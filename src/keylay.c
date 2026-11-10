/*
 * KeyLay: reports the active layer and the battery levels to the KeyLay menu bar app
 * through a small GATT service. See firmware/PROTOCOL.md in the KeyLay project.
 *
 * The state characteristic is readable and sends a notification whenever the layer state or
 * a battery level changes. The &keylay behavior (the KeyLay key in the keymap) sends a
 * command on the same characteristic that opens or closes the app's cheat sheet. On a split keyboard this runs on the central only; peripheral
 * battery levels need CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define KEYLAY_MAGIC 0x4B
#define KEYLAY_VERSION 0x01
#define KEYLAY_MSG_STATE 0x01
#define KEYLAY_MSG_COMMAND 0x03
#define KEYLAY_CMD_TOGGLE_POPOVER 0x01
#define KEYLAY_BATTERY_UNKNOWN 0xFF
#define KEYLAY_HEADER_LEN 9
#define KEYLAY_MAX_BATTERIES 4
#define KEYLAY_MAX_LEN (KEYLAY_HEADER_LEN + KEYLAY_MAX_BATTERIES)

/* Retry delay when the Bluetooth stack has no buffer for a notification */
#define KEYLAY_RETRY_MS 100

#define KEYLAY_SERVICE_UUID                                                                       \
    BT_UUID_128_ENCODE(0x9f3c0001, 0x5b2e, 0x4a6d, 0x8f1c, 0x3e7a2b9d4c60)
#define KEYLAY_STATE_UUID                                                                         \
    BT_UUID_128_ENCODE(0x9f3c0002, 0x5b2e, 0x4a6d, 0x8f1c, 0x3e7a2b9d4c60)

static size_t keylay_build_state(uint8_t *buf) {
    buf[0] = KEYLAY_MAGIC;
    buf[1] = KEYLAY_VERSION;
    buf[2] = KEYLAY_MSG_STATE;
    buf[3] = zmk_keymap_highest_layer_active();
    sys_put_le32(zmk_keymap_layer_state(), &buf[4]);

    uint8_t *levels = &buf[KEYLAY_HEADER_LEN];
    uint8_t count = 0;

    /* Battery 0: this device (the keyboard, or the split central / dongle) */
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    levels[count++] = zmk_battery_state_of_charge();
#else
    levels[count++] = KEYLAY_BATTERY_UNKNOWN;
#endif

    /* Batteries 1..: split peripherals. A level of 0 means nothing was received yet. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    for (uint8_t source = 0;
         source < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT && count < KEYLAY_MAX_BATTERIES; source++) {
        uint8_t level = 0;
        int err = zmk_split_central_get_peripheral_battery_level(source, &level);
        levels[count++] = (err == 0 && level > 0) ? level : KEYLAY_BATTERY_UNKNOWN;
    }
#endif

    buf[8] = count;
    return KEYLAY_HEADER_LEN + count;
}

static ssize_t keylay_read_state(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                  uint16_t len, uint16_t offset) {
    uint8_t state[KEYLAY_MAX_LEN];
    size_t state_len = keylay_build_state(state);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, state, state_len);
}

static void keylay_notify_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(keylay_notify_work, keylay_notify_work_cb);

static void keylay_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    if (value == BT_GATT_CCC_NOTIFY) {
        /* Send the current state right away to a host that just subscribed */
        k_work_reschedule(&keylay_notify_work, K_NO_WAIT);
    }
}

BT_GATT_SERVICE_DEFINE(keylay_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(KEYLAY_SERVICE_UUID)),
                       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(KEYLAY_STATE_UUID),
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ_ENCRYPT, keylay_read_state, NULL,
                                              NULL),
                       BT_GATT_CCC(keylay_ccc_changed,
                                   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static void keylay_notify_work_cb(struct k_work *work) {
    uint8_t state[KEYLAY_MAX_LEN];
    size_t state_len = keylay_build_state(state);

    /* NULL: every connection that subscribed. attrs[1] is the characteristic declaration. */
    int err = bt_gatt_notify(NULL, &keylay_svc.attrs[1], state, state_len);
    if (err == -ENOMEM || err == -ENOBUFS) {
        k_work_reschedule(&keylay_notify_work, K_MSEC(KEYLAY_RETRY_MS));
    } else if (err && err != -ENOTCONN) {
        LOG_DBG("KeyLay notification failed: %d", err);
    }
}

/* The KeyLay key: asks the app to open or close its cheat sheet */
static void keylay_command_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(keylay_command_work, keylay_command_work_cb);

static void keylay_command_work_cb(struct k_work *work) {
    uint8_t msg[] = {KEYLAY_MAGIC, KEYLAY_VERSION, KEYLAY_MSG_COMMAND, KEYLAY_CMD_TOGGLE_POPOVER};
    int err = bt_gatt_notify(NULL, &keylay_svc.attrs[1], msg, sizeof(msg));
    if (err == -ENOMEM || err == -ENOBUFS) {
        k_work_reschedule(&keylay_command_work, K_MSEC(KEYLAY_RETRY_MS));
    } else if (err && err != -ENOTCONN) {
        LOG_DBG("KeyLay command failed: %d", err);
    }
}

#define DT_DRV_COMPAT zmk_behavior_keylay

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int keylay_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    k_work_reschedule(&keylay_command_work, K_NO_WAIT);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int keylay_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api keylay_driver_api = {
    .binding_pressed = keylay_binding_pressed,
    .binding_released = keylay_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &keylay_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

static int keylay_listener(const zmk_event_t *eh) {
    /* Coalesces bursts (e.g. a tri-layer change): the work item sends the latest state */
    k_work_reschedule(&keylay_notify_work, K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(keylay, keylay_listener);
ZMK_SUBSCRIPTION(keylay, zmk_layer_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_SUBSCRIPTION(keylay, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(keylay, zmk_peripheral_battery_state_changed);
#endif
#endif
