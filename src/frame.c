#include <string.h>

#include "frame.h"

uint16_t sntl_crc16(uint16_t seed, const uint8_t *data, size_t len)
{
	uint16_t crc = seed;

	while (len--) {
		crc ^= (uint16_t)(*data++) << 8;
		for (int i = 0; i < 8; i++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
					      : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

void sntl_encode_header(uint8_t *out, uint16_t record_count, const char *device_id)
{
	memcpy(out, SNTL_MAGIC, 4);
	out[4] = SNTL_VERSION;
	out[5] = SNTL_RECORD_SIZE;
	out[6] = (uint8_t)(record_count & 0xFF);
	out[7] = (uint8_t)(record_count >> 8);

	/* zero padded, never NUL-terminated-and-then-garbage */
	memset(out + 8, 0, SNTL_DEVICE_ID_LEN);
	for (size_t i = 0; i < SNTL_DEVICE_ID_LEN && device_id[i] != '\0'; i++) {
		out[8 + i] = (uint8_t)device_id[i];
	}
}

void sntl_encode_record(uint8_t *out, const struct sntl_record *rec)
{
	out[0]  = (uint8_t)(rec->seq);
	out[1]  = (uint8_t)(rec->seq >> 8);
	out[2]  = (uint8_t)(rec->seq >> 16);
	out[3]  = (uint8_t)(rec->seq >> 24);
	out[4]  = (uint8_t)(rec->ts);
	out[5]  = (uint8_t)(rec->ts >> 8);
	out[6]  = (uint8_t)(rec->ts >> 16);
	out[7]  = (uint8_t)(rec->ts >> 24);
	out[8]  = (uint8_t)(rec->pm25);
	out[9]  = (uint8_t)(rec->pm25 >> 8);
	out[10] = (uint8_t)(rec->pm10);
	out[11] = (uint8_t)(rec->pm10 >> 8);
	out[12] = rec->flags;
	out[13] = rec->batt;
}

size_t sntl_frame_size(uint16_t record_count)
{
	return SNTL_HEADER_SIZE + (size_t)record_count * SNTL_RECORD_SIZE + SNTL_CRC_SIZE;
}

int sntl_parse_cmd(const uint8_t *buf, size_t len, uint32_t *first_seq)
{
	if (len != SNTL_CMD_LEN || buf[0] != SNTL_CMD_DUMP) {
		return 0;
	}
	*first_seq = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) |
		     ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
	return 1;
}

uint16_t sntl_dump_range(uint32_t first_seq, uint32_t oldest_seq, uint32_t next_seq,
			 uint32_t *start_seq)
{
	uint32_t start = (first_seq > oldest_seq) ? first_seq : oldest_seq;
	uint32_t count;

	if (start >= next_seq) {
		*start_seq = next_seq;
		return 0;
	}
	count = next_seq - start;
	if (count > UINT16_MAX) {
		count = UINT16_MAX;   /* recordCount is uint16 on the wire */
	}
	*start_seq = start;
	return (uint16_t)count;
}
