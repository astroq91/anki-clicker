/*
 * Based on sample by Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>
#include <assert.h>
#include <zephyr/spinlock.h>

#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/bluetooth/services/bas.h>
#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/services/dis.h>


#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define BASE_USB_HID_SPEC_VERSION   0x0101
#define INPUT_REP_KEYS_REF_ID            0
#define OUTPUT_REPORT_MAX_LEN            1
#define HIDS_QUEUE_SIZE 10

/* ********************* */
/* Buttons configuration */

/* Note: The configuration below is the same as BOOT mode configuration
 * This simplifies the code as the BOOT mode is the same as REPORT mode.
 * Changing this configuration would require separate implementation of
 * BOOT mode report generation.
 */
#define KEY_CTRL_CODE_MIN 224 /* Control key codes - required 8 of them */
#define KEY_CTRL_CODE_MAX 231 /* Control key codes - required 8 of them */
#define KEY_CODE_MIN      0   /* Normal key codes */
#define KEY_CODE_MAX      101 /* Normal key codes */
#define KEY_PRESS_MAX     6   /* Maximum number of non-control keys
			       * pressed simultaneously
			       */

#define INPUT_REPORT_KEYS_MAX_LEN (1 + 1 + KEY_PRESS_MAX)

/* HIDS instance. */
BT_HIDS_DEF(hids_obj,
	    OUTPUT_REPORT_MAX_LEN,
	    INPUT_REPORT_KEYS_MAX_LEN);

static volatile bool is_adv;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
					  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

struct bt_conn *current_conn;
bool in_boot_mode;


static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = NULL,
	.passkey_confirm = NULL,
	.cancel = NULL,
};

static void advertising_start(void)
{
	int err;
	const struct bt_le_adv_param *adv_param = BT_LE_ADV_PARAM(
						BT_LE_ADV_OPT_CONN,
						BT_GAP_ADV_FAST_INT_MIN_2,
						BT_GAP_ADV_FAST_INT_MAX_2,
						NULL);

	err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd,
			      ARRAY_SIZE(sd));
	if (err) {
		if (err == -EALREADY) {
			printk("Advertising continued\n");
		} else {
			printk("Advertising failed to start (err %d)\n", err);
		}

		return;
	}

	is_adv = true;
	printk("Advertising successfully started\n");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("Failed to connect to %s 0x%02x %s\n", addr, err, bt_hci_err_to_str(err));
		return;
	}
	printk("Connected %s\n", addr);

	err = bt_hids_connected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about connection\n");
		return;
	}

	current_conn = conn;
	is_adv = false;

  bt_conn_set_security(conn, BT_SECURITY_L2);
}


static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	int err;
	bool is_any_dev_connected = false;
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected from %s, reason 0x%02x %s\n", addr, reason, bt_hci_err_to_str(reason));

	err = bt_hids_disconnected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about disconnection\n");
	}

  current_conn = NULL;

	advertising_start();
}


static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("Security changed: %s level %u\n", addr, level);
	} else {
		printk("Security failed: %s level %u err %d %s\n", addr, level, err,
		       bt_security_err_to_str(err));
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}
static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("Pairing failed conn: %s, reason %d %s\n", addr, reason,
           bt_security_err_to_str(reason));
}

static const uint8_t report_map[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x06, /* Usage (Keyboard) */
    0xA1, 0x01, /* Collection (Application) */

    /* Modifier keys (1 byte) */
    0x05, 0x07, /* Usage Page (Key Codes) */
    0x19, 0xE0, /* Usage Minimum (224) */
    0x29, 0xE7, /* Usage Maximum (231) */
    0x15, 0x00, /* Logical Minimum (0) */
    0x25, 0x01, /* Logical Maximum (1) */
    0x75, 0x01, /* Report Size (1) */
    0x95, 0x08, /* Report Count (8) */
    0x81, 0x02, /* Input (Data, Variable, Absolute) */

    /* Reserved byte */
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01, /* Input (Constant) */

    /* Key array (6 bytes) */
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00, /* Input (Data, Array) */

    /* LED output (1 byte) */
    0x05, 0x08,
    0x19, 0x01,
    0x29, 0x05,
    0x95, 0x05,
    0x75, 0x01,
    0x91, 0x02, /* Output (Data, Variable, Absolute) */
    0x95, 0x01,
    0x75, 0x03,
    0x91, 0x01, /* Output (Constant) - padding */

    0xC0,       /* End Collection */
};

static void hid_init(void)
{
	int err;
	struct bt_hids_init_param    hids_init_obj = { 0 };
	struct bt_hids_inp_rep       *hids_inp_rep;
	struct bt_hids_outp_feat_rep *hids_outp_rep;

  
  hids_init_obj.rep_map.data = report_map;
  hids_init_obj.rep_map.size = sizeof(report_map);

  hids_init_obj.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
  hids_init_obj.info.b_country_code = 0x00;
  hids_init_obj.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;

  hids_inp_rep = &hids_init_obj.inp_rep_group_init.reports[0];
  hids_inp_rep->size = INPUT_REPORT_KEYS_MAX_LEN;
  hids_inp_rep->id = INPUT_REP_KEYS_REF_ID;
  hids_init_obj.inp_rep_group_init.cnt = 1;

  hids_init_obj.is_kb = true;

  err = bt_hids_init(&hids_obj, &hids_init_obj);

	__ASSERT(err == 0, "HIDS initialization failed\n");
}


static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
  .pairing_complete = pairing_complete, 
  .pairing_failed = pairing_failed,
};

int main(void) {
  int err;
  //
  // err = bt_conn_auth_cb_register(&conn_auth_callbacks);
  // if (err) {
  //   printk("Failed to register authorization callbacks.\n");
  //   return 0;
  // }

  err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
  if (err) {
    printk("Failed to register info callback.\n");
    return 0;
  }

  err = bt_enable(NULL);
  if (err) {
    printk("Failed to enable bluetooth.\n");
    return 0;
  }
  settings_load();
  bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
  hid_init();
  advertising_start();
  
  for (;;) {
    if (current_conn) {
      uint8_t report[8] = {0};
      report[2] = 0x04;
      bt_hids_inp_rep_send(&hids_obj, current_conn, 0, report, sizeof(report), NULL);
      memset(report, 0, sizeof(report));
      bt_hids_inp_rep_send(&hids_obj, current_conn, 0, report, sizeof(report), NULL);
    }
    k_sleep(K_MSEC(100));
  }
}

