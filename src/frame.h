/*
 * SNTL wire frame - node to app.
 *
 * Deliberately plain C (stdint/string only, no Zephyr) so tests/test_frame.c
 * can build and run on the host. Do not add Zephyr includes here.
 *
 * Frame layout, all integers little-endian:
 *   header 16 B | record 14 B x recordCount | crc16 2 B
 * The CRC covers everything from the first header byte up to (not including)
 * the CRC itself.
 */
#ifndef FRAME_H_
#define FRAME_H_

#include <stdint.h>
#include <stddef.h>

#define SNTL_MAGIC          "SNTL"
#define SNTL_VERSION        1
#define SNTL_RECORD_SIZE    14
#define SNTL_HEADER_SIZE    16
#define SNTL_CRC_SIZE       2
#define SNTL_DEVICE_ID_LEN  8

/* Control characteristic command: 0x01 then uint32 LE firstSeq. */
#define SNTL_CMD_DUMP       0x01
#define SNTL_CMD_LEN        5

/* record flags */
#define SNTL_FLAG_SENSOR_FAULT        0x01
#define SNTL_FLAG_LOW_BATTERY         0x02
#define SNTL_FLAG_RTC_UNSET           0x04
#define SNTL_FLAG_CALIBRATING         0x08
#define SNTL_FLAG_THRESHOLD_EXCEEDED  0x10

/* Node-side threshold that raises SNTL_FLAG_THRESHOLD_EXCEEDED. */
#define SNTL_PM10_THRESHOLD  150

struct sntl_record {
	uint32_t seq;
	uint32_t ts;      /* UTC unix seconds; RTC_UNSET flag if not trustworthy */
	uint16_t pm25;    /* ug/m3, integer */
	uint16_t pm10;    /* ug/m3, integer */
	uint8_t  flags;
	uint8_t  batt;    /* percent, 0..100 */
};

/*
 * CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR.
 * Call with seed 0xFFFF and chain across chunks to CRC a streamed frame.
 * Check vector: "123456789" -> 0x29B1.
 */
uint16_t sntl_crc16(uint16_t seed, const uint8_t *data, size_t len);

/* out must be SNTL_HEADER_SIZE bytes. device_id is ASCII, zero padded to 8. */
void sntl_encode_header(uint8_t *out, uint16_t record_count, const char *device_id);

/* out must be SNTL_RECORD_SIZE bytes. */
void sntl_encode_record(uint8_t *out, const struct sntl_record *rec);

/* Total frame length for a given record count. */
size_t sntl_frame_size(uint16_t record_count);

/* Parse a control write. Returns 1 and sets *first_seq on a valid dump
 * command, 0 otherwise. */
int sntl_parse_cmd(const uint8_t *buf, size_t len, uint32_t *first_seq);

/*
 * How many records a dump answers with, and where it starts.
 * The selection is seq >= first_seq (inclusive - an app with nothing stored
 * sends first_seq = 0 and must still get record 0), clamped to what the ring
 * still holds. Returns the count; *start_seq gets the first seq to send.
 * Kept here rather than in the BLE code so the boundaries are host-testable.
 */
uint16_t sntl_dump_range(uint32_t first_seq, uint32_t oldest_seq, uint32_t next_seq,
			 uint32_t *start_seq);

#endif /* FRAME_H_ */
