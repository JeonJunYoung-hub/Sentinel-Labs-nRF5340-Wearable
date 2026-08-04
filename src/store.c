#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/kvss/nvs.h>
#include <errno.h>
#include <string.h>

#include "store.h"

/*
 * One NVS entry per record, id = NVS_ID_REC0 + (seq % STORE_CAPACITY), so the
 * ring drops the oldest for free. Deliberately NOT batching writes: at 14 B of
 * payload plus an 8 B NVS ATE, a full day is 1440 * 22 = ~31 KB, and with the
 * 160 KB partition below that works out to roughly 8 sector erases a day
 * spread over 40 sectors - a few hundred erase cycles a year against a 10k
 * endurance rating. Batching would only buy wear we do not need, and would
 * cost up to a batch of records on power loss.
 */
#define NVS_ID_META   1
#define NVS_ID_REC0   0x100

struct meta {
	uint32_t next_seq;
	uint32_t base_sec;   /* device seconds at the last append */
};

static struct nvs_fs fs;
static struct meta meta;
static uint32_t boot_offset;   /* device seconds at this boot */
K_MUTEX_DEFINE(lock);   /* main loop appends, BLE thread reads */

uint32_t store_now(void)
{
	return boot_offset + (uint32_t)(k_uptime_get() / 1000);
}

int store_init(void)
{
	struct flash_pages_info info;
	int err;

	fs.flash_device = PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(fs.flash_device)) {
		return -ENODEV;
	}
	fs.offset = PARTITION_OFFSET(storage_partition);
	err = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
	if (err) {
		return err;
	}
	fs.sector_size = info.size;
	fs.sector_count = PARTITION_SIZE(storage_partition) / info.size;

	err = nvs_mount(&fs);
	if (err) {
		return err;
	}

	if (nvs_read(&fs, NVS_ID_META, &meta, sizeof(meta)) != sizeof(meta)) {
		meta.next_seq = 0;
		meta.base_sec = 0;
	}
	/* Resume the clock where the last append left it. The powered-off gap
	 * is unknowable without an RTC - that is what RTC_UNSET is for. */
	boot_offset = meta.base_sec;
	return 0;
}

uint32_t store_next_seq(void)
{
	return meta.next_seq;
}

uint32_t store_oldest_seq(void)
{
	return (meta.next_seq > STORE_CAPACITY) ? meta.next_seq - STORE_CAPACITY : 0;
}

int store_append(struct sntl_record *rec)
{
	int err;

	k_mutex_lock(&lock, K_FOREVER);

	rec->seq = meta.next_seq;
	rec->ts  = store_now();

	err = nvs_write(&fs, NVS_ID_REC0 + (rec->seq % STORE_CAPACITY), rec, sizeof(*rec));
	if (err < 0) {
		k_mutex_unlock(&lock);
		return err;
	}

	meta.next_seq++;
	meta.base_sec = rec->ts;
	err = nvs_write(&fs, NVS_ID_META, &meta, sizeof(meta));

	k_mutex_unlock(&lock);
	return (err < 0) ? err : 0;
}

int store_get(uint32_t seq, struct sntl_record *out)
{
	ssize_t n;

	if (seq >= meta.next_seq || seq < store_oldest_seq()) {
		return -ENOENT;
	}

	k_mutex_lock(&lock, K_FOREVER);
	n = nvs_read(&fs, NVS_ID_REC0 + (seq % STORE_CAPACITY), out, sizeof(*out));
	k_mutex_unlock(&lock);

	if (n != sizeof(*out)) {
		return -ENOENT;
	}
	/* Slot may already have been recycled by a newer seq mid-read. */
	return (out->seq == seq) ? 0 : -ENOENT;
}
