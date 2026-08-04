/*
 * Flash-backed ring of measurement records.
 *
 * Holds 24 h at one record per minute. seq is monotonic and survives reset -
 * the app and server dedupe on (device_id, seq), so restarting at 0 would make
 * new data collide with old and get dropped.
 */
#ifndef STORE_H_
#define STORE_H_

#include <stdint.h>

#include "frame.h"

/*
 * TEST BUILD: 30 s instead of the 60 s the app and server assume. Put this
 * back to 60 before shipping, or change the app's samplePeriod and the
 * server's SAMPLE_PERIOD_SECONDS to match - they compute "time over
 * threshold" by multiplying record counts by this.
 */
#define STORE_PERIOD_S   30
#define STORE_CAPACITY   (24 * 3600 / STORE_PERIOD_S)   /* always 24 h */

int store_init(void);

/* Fills in seq and ts, then persists. Safe to call from any thread. */
int store_append(struct sntl_record *rec);

/* seq that will be handed to the next appended record. */
uint32_t store_next_seq(void);

/* Oldest seq still in the ring (0 until it has wrapped). */
uint32_t store_oldest_seq(void);

/* 0 on success, -ENOENT if that seq is no longer held. */
int store_get(uint32_t seq, struct sntl_record *out);

/* Device seconds since first ever boot. Not wall clock - records carry
 * SNTL_FLAG_RTC_UNSET until something sets the time. */
uint32_t store_now(void);

#endif /* STORE_H_ */
