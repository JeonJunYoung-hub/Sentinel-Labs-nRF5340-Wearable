/*
 * Zephyr HAL for the Sensirion embedded-i2c-sen5x library.
 * 라이브러리(sen5x_i2c.c 등)가 요구하는 5개 함수만 Zephyr I2C로 매핑한다.
 * 센서 버스는 devicetree 오버레이의 &i2c1 하위 sen54 노드에서 가져온다.
 */
#include "sensirion_i2c_hal.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>

/* 오버레이의 sen54: sen54@69 노드 → { .bus = i2c1 디바이스, .addr = 0x69 } */
static const struct i2c_dt_spec sen54 = I2C_DT_SPEC_GET(DT_NODELABEL(sen54));

void sensirion_i2c_hal_init(void)
{
	/* 준비 여부는 main.c에서 device_is_ready()로 확인. 여기선 할 일 없음. */
}

void sensirion_i2c_hal_free(void)
{
	/* Zephyr I2C 디바이스는 정적 — 해제할 자원 없음. */
}

int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t *data, uint16_t count)
{
	/* address는 라이브러리가 0x69를 넘겨줌 (sen54.addr와 동일) */
	return i2c_read(sen54.bus, data, count, address) == 0 ? 0 : -1;
}

int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t *data, uint16_t count)
{
	return i2c_write(sen54.bus, data, count, address) == 0 ? 0 : -1;
}

void sensirion_i2c_hal_sleep_usec(uint32_t useconds)
{
	k_usleep((int32_t)useconds);
}
