#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ble.h"
#include "store.h"

/* NUS-shaped UUIDs but this is not NUS - the app keys off these exact values. */
#define SNTL_UUID_SVC  BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
#define SNTL_UUID_CTRL BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
#define SNTL_UUID_DATA BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

static const struct bt_uuid_128 uuid_svc  = BT_UUID_INIT_128(SNTL_UUID_SVC);
static const struct bt_uuid_128 uuid_ctrl = BT_UUID_INIT_128(SNTL_UUID_CTRL);
static const struct bt_uuid_128 uuid_data = BT_UUID_INIT_128(SNTL_UUID_DATA);

static char device_id[SNTL_DEVICE_ID_LEN + 1];
static struct bt_conn *current_conn;
static volatile bool notify_enabled;

/* Dump runs on its own thread: it can take seconds over the air, and it must
 * not sit on the BLE RX callback or on the system workqueue (the buzzer melody
 * lives there). */
#define DUMP_STACK_SIZE 2048
#define DUMP_PRIORITY   7

K_SEM_DEFINE(dump_req, 0, 1);
static uint32_t dump_first_seq;

const char *ble_device_id(void)
{
	return device_id;
}

/* "SL" + 6 hex from the factory device id = exactly 8 bytes, so the frame
 * field and the advertising name are the same string with no truncation. */
static void device_id_init(void)
{
	uint8_t hw[8] = {0};
	ssize_t n = hwinfo_get_device_id(hw, sizeof(hw));
	uint32_t tail;

	if (n < 3) {
		tail = 0;
	} else {
		tail = ((uint32_t)hw[n - 3] << 16) | ((uint32_t)hw[n - 2] << 8) | hw[n - 1];
	}
	snprintf(device_id, sizeof(device_id), "SL%06X", (unsigned int)(tail & 0xFFFFFF));
}

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SNTL_UUID_SVC),
};

/* Name goes in the scan response - the 128-bit UUID already fills the 31 byte
 * advertising payload. */
static struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, device_id, SNTL_DEVICE_ID_LEN),
};

static ssize_t ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint32_t first_seq;

	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (!sntl_parse_cmd(buf, len, &first_seq)) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	dump_first_seq = first_seq;
	k_sem_give(&dump_req);
	printk("[BLE  ] dump requested from seq %u\n", first_seq);
	return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(sntl_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),
	BT_GATT_CHARACTERISTIC(&uuid_ctrl.uuid,
			       BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
			       NULL, ctrl_write, NULL),
	BT_GATT_CHARACTERISTIC(&uuid_data.uuid,
			       BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

#define DATA_ATTR (&sntl_svc.attrs[3])

/* Push one chunk, retrying while the controller's buffers are full. */
static int notify_chunk(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
	int err;

	while ((err = bt_gatt_notify(conn, DATA_ATTR, data, len)) == -ENOMEM) {
		k_msleep(2);
		if (!current_conn) {
			return -ENOTCONN;
		}
	}
	return err;
}

/*
 * Stream one frame: header, then records, then CRC. The frame is emitted
 * straight into MTU-sized notifications with no per-chunk framing - the app
 * concatenates and uses the header's record count to find the end. A full day
 * is ~20 KB, so it is built incrementally rather than buffered.
 */
static void dump_run(struct bt_conn *conn, uint32_t first_seq)
{
	uint8_t buf[244];
	uint16_t mtu, chunk, fill = 0;
	uint16_t crc = 0xFFFF;
	uint32_t start, seq;
	uint16_t count;

	count = sntl_dump_range(first_seq, store_oldest_seq(), store_next_seq(), &start);

	mtu = bt_gatt_get_mtu(conn);
	chunk = (mtu > 3) ? (uint16_t)(mtu - 3) : 20;
	if (chunk > sizeof(buf)) {
		chunk = sizeof(buf);
	}

	/* header */
	sntl_encode_header(buf, count, device_id);
	fill = SNTL_HEADER_SIZE;
	crc = sntl_crc16(crc, buf, fill);

	for (seq = start; seq < start + count; seq++) {
		struct sntl_record rec;

		if (store_get(seq, &rec) != 0) {
			/* Recycled under us - send zeros rather than desync the
			 * record count the app is counting down. */
			memset(&rec, 0, sizeof(rec));
			rec.seq = seq;
			rec.flags = SNTL_FLAG_SENSOR_FAULT;
		}

		if (fill + SNTL_RECORD_SIZE > chunk) {
			if (notify_chunk(conn, buf, fill) < 0) {
				return;
			}
			fill = 0;
		}
		sntl_encode_record(buf + fill, &rec);
		crc = sntl_crc16(crc, buf + fill, SNTL_RECORD_SIZE);
		fill += SNTL_RECORD_SIZE;
	}

	/* trailing CRC, split across a chunk boundary if it lands there */
	if (fill + SNTL_CRC_SIZE > chunk) {
		if (notify_chunk(conn, buf, fill) < 0) {
			return;
		}
		fill = 0;
	}
	buf[fill++] = (uint8_t)(crc & 0xFF);
	buf[fill++] = (uint8_t)(crc >> 8);

	if (notify_chunk(conn, buf, fill) < 0) {
		return;
	}
	printk("[BLE  ] dump sent: %u records from seq %u\n", count, start);
}

static void dump_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (1) {
		k_sem_take(&dump_req, K_FOREVER);
		if (current_conn && notify_enabled) {
			dump_run(current_conn, dump_first_seq);
		}
	}
}

K_THREAD_DEFINE(dump_tid, DUMP_STACK_SIZE, dump_thread, NULL, NULL, NULL,
		DUMP_PRIORITY, 0, 0);

/* ---------------- advertising window ----------------
 * Connectable advertising stops by itself the moment a central connects, so
 * without the restart below the node would be invisible after the first
 * session until the next reset. The window bounds how long it stays visible. */
static volatile bool adv_window_open;

static void adv_start(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));

	if (err == -EALREADY) {
		return;
	}
	if (err) {
		printk("[BLE  ] adv start failed (%d)\n", err);
		return;
	}
	printk("[BLE  ] advertising as %s for %d s\n", device_id, BLE_ADV_WINDOW_S);
}

static void adv_on_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	adv_start();
}
static K_WORK_DEFINE(adv_on_work, adv_on_fn);

static void adv_off_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	adv_window_open = false;
	bt_le_adv_stop();   /* does not drop an established connection */
	printk("[BLE  ] advertising window closed\n");
}
static K_WORK_DELAYABLE_DEFINE(adv_off_work, adv_off_fn);

void ble_advertise_window(void)
{
	adv_window_open = true;
	k_work_submit(&adv_on_work);
	k_work_reschedule(&adv_off_work, K_SECONDS(BLE_ADV_WINDOW_S));
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("[BLE  ] connect failed (%u)\n", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	printk("[BLE  ] connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	printk("[BLE  ] disconnected (0x%02x)\n", reason);
	notify_enabled = false;
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	if (adv_window_open) {
		k_work_submit(&adv_on_work);   /* still inside the window */
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

int ble_start(void)
{
	int err;

	device_id_init();

	err = bt_enable(NULL);
	if (err) {
		return err;
	}

	err = bt_set_name(device_id);
	if (err) {
		printk("[BLE  ] bt_set_name failed (%d)\n", err);
	}

	printk("[READY] BLE up as %s - press Button 1 to advertise\n", device_id);
	return 0;
}
