#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>

#include "sen5x_i2c.h"
#include "sensirion_i2c_hal.h"

#include "bmv080.h"
#include "bmv080_defs.h"

/* All sensors share i2c1 (SEN54=0x69, BMV080=0x54, SHT40=0x44) */
static const struct i2c_dt_spec sen54 = I2C_DT_SPEC_GET(DT_NODELABEL(sen54));
static const struct i2c_dt_spec bmv080_spec = I2C_DT_SPEC_GET(DT_NODELABEL(bmv080));
/* SHT40 uses the mainline Zephyr sht4x driver via the sensor API */
static const struct device *const sht40 = DEVICE_DT_GET(DT_NODELABEL(sht40));

/* ---------------- BMV080 sercom callbacks (Zephyr I2C) ----------------
 * Protocol (datasheet 4.4.2.4): 16-bit header then N 16-bit big-endian words.
 * The header's MSB is the R/W bit (already folded into the I2C address by the
 * driver), so we shift the header left by 1 and send it as a 2-byte big-endian
 * "register address", then transfer payload words big-endian. Ref: Bosch STM32
 * example (HAL_I2C_Mem_Read/Write with header<<1). */
static int8_t bmv080_read(bmv080_sercom_handle_t h, uint16_t header,
			  uint16_t *payload, uint16_t len)
{
	const struct i2c_dt_spec *s = h;
	uint16_t ha = header << 1;
	uint8_t hb[2] = { (uint8_t)(ha >> 8), (uint8_t)(ha & 0xFF) };

	if (i2c_write_read(s->bus, s->addr, hb, 2, payload, (size_t)len * 2) != 0) {
		return -1;
	}
	/* big-endian wire -> host: swap each 16-bit word */
	for (uint16_t i = 0; i < len; i++) {
		uint16_t w = payload[i];
		payload[i] = (uint16_t)((w << 8) | (w >> 8));
	}
	return 0;
}

static int8_t bmv080_write(bmv080_sercom_handle_t h, uint16_t header,
			   const uint16_t *payload, uint16_t len)
{
	const struct i2c_dt_spec *s = h;
	uint8_t buf[2 + 2 * 32];   /* driver writes are small (config words) */

	if ((size_t)len * 2 + 2 > sizeof(buf)) {
		return -1;
	}
	uint16_t ha = header << 1;
	buf[0] = (uint8_t)(ha >> 8);
	buf[1] = (uint8_t)(ha & 0xFF);
	for (uint16_t i = 0; i < len; i++) {
		buf[2 + i * 2]     = (uint8_t)(payload[i] >> 8);   /* big-endian */
		buf[2 + i * 2 + 1] = (uint8_t)(payload[i] & 0xFF);
	}
	return i2c_write(s->bus, buf, (size_t)len * 2 + 2, s->addr) == 0 ? 0 : -1;
}

static int8_t bmv080_delay(uint32_t ms)
{
	k_msleep((int32_t)ms);
	return 0;
}

/* Air-quality grade by ug/m3 thresholds (Korean AQI style, labels ASCII so the
 * serial terminal doesn't garble multibyte). */
static const char *aq_grade(float v, float good, float moderate, float bad)
{
	if (v <= good)     return "GOOD";
	if (v <= moderate) return "MODERATE";
	if (v <= bad)      return "BAD";
	return "VERY BAD";
}

/* High-PM exposure warning: PM10>=150 or PM2.5>=75 sustained for 2 hours. */
#define AQ_PM10_HIGH   150.0f
#define AQ_PM25_HIGH   75.0f
#define AQ_WARN_MS     (2LL * 3600 * 1000)   /* 2 hours */

/* Called by bmv080_serve_interrupt() when a new measurement is ready.
 * Values are float ug/m3; scale x100 to print without float printf support. */
static void bmv080_data_ready(bmv080_output_t out, void *param)
{
	static int64_t high_since = -1;   /* uptime ms when high-PM started, -1 = not high */

	int pm1  = (int)(out.pm1_mass_concentration  * 100.0f);
	int pm25 = (int)(out.pm2_5_mass_concentration * 100.0f);
	int pm10 = (int)(out.pm10_mass_concentration * 100.0f);

	printk("\n\n--------------- BMV080 MEASUREMENT -----\n");
	printk("  PM1.0 : %d.%02d ug/m3\n", pm1 / 100, (pm1 < 0 ? -pm1 : pm1) % 100);
	printk("  PM2.5 : %d.%02d ug/m3  [%s]\n", pm25 / 100, (pm25 < 0 ? -pm25 : pm25) % 100,
	       aq_grade(out.pm2_5_mass_concentration, 15.0f, 35.0f, 75.0f));
	printk("  PM10  : %d.%02d ug/m3  [%s]\n", pm10 / 100, (pm10 < 0 ? -pm10 : pm10) % 100,
	       aq_grade(out.pm10_mass_concentration, 30.0f, 80.0f, 150.0f));

	if (out.is_obstructed) {
		printk("  [warn] sensor obstructed\n");
	}

	/* Sustained high-PM tracking */
	bool high = (out.pm10_mass_concentration >= AQ_PM10_HIGH) ||
		    (out.pm2_5_mass_concentration >= AQ_PM25_HIGH);
	if (high) {
		if (high_since < 0) {
			high_since = k_uptime_get();
		}
		int dur_s = (int)((k_uptime_get() - high_since) / 1000);
		printk("  [HIGH ] PM10>=150 or PM2.5>=75  (sustained %dh%02dm / 2h)\n",
		       dur_s / 3600, (dur_s % 3600) / 60);
		if ((k_uptime_get() - high_since) >= AQ_WARN_MS) {
			printk("  *** WARNING: high PM exposure >= 2h ***\n");
		}
	} else {
		high_since = -1;   /* condition broke -> reset the timer */
	}
}

int main(void)
{
	int16_t err;
	bmv080_handle_t bmv = NULL;

	printk("\n");
	printk("========================================\n");
	printk("   Air Quality: SEN54 + BMV080\n");
	printk("========================================\n");

	if (!device_is_ready(sen54.bus)) {
		printk("[ERROR] I2C bus not ready\n");
		return 0;
	}
	sensirion_i2c_hal_init();

	/* ---------------- SEN54 ---------------- */
	printk("[INIT ] SEN54 reset...\n");
	sen5x_device_reset();
	k_msleep(100);   /* SEN5x needs up to 100 ms to be ready after reset */

	/* Poll until the sensor answers. Lets you fix wiring live without reflash. */
	unsigned char serial[32];
	while ((err = sen5x_get_serial_number(serial, sizeof(serial))) != 0) {
		printk("[WAIT ] SEN54 no response (err %d) - check 5V / SEL->GND / pull-ups / wiring\n",
		       err);
		k_msleep(1000);
	}
	printk("[READY] SEN54 (serial: %s)\n", serial);

	err = sen5x_start_measurement();
	if (err) {
		printk("[ERROR] SEN54 start failed (err %d)\n", err);
		return 0;
	}
	printk("[START] SEN54 measuring\n");

	/* ---------------- BMV080 ----------------
	 * open() talks to the sensor; if BMV080 isn't wired it fails and we keep
	 * running SEN54 only. */
	bmv080_status_code_t bst = bmv080_open(&bmv, (bmv080_sercom_handle_t)&bmv080_spec,
					       bmv080_read, bmv080_write, bmv080_delay);
	if (bst != E_BMV080_OK) {
		printk("[WARN ] BMV080 open failed (%d) - check wiring/PS pin, running SEN54 only\n",
		       (int)bst);
		bmv = NULL;
	} else {
		char id[13] = {0};
		if (bmv080_get_sensor_id(bmv, id) == E_BMV080_OK) {
			printk("[READY] BMV080 (id: %s)\n", id);
		}
		if (bmv080_start_continuous_measurement(bmv) == E_BMV080_OK) {
			printk("[START] BMV080 measuring\n");
		}
	}

	/* ---------------- SHT40 (mainline sensor driver) ---------------- */
	if (device_is_ready(sht40)) {
		printk("[READY] SHT40\n");
	} else {
		printk("[WARN ] SHT40 not ready - check wiring, skipping it\n");
	}

	while (1) {
		k_msleep(1000);   /* BMV080 must be served at least once per second */

		/* --- BMV080: poll for new data (prints via data_ready cb) --- */
		if (bmv != NULL) {
			bmv080_serve_interrupt(bmv, bmv080_data_ready, NULL);
		}

		/* --- SEN54 --- */
		uint16_t pm1p0, pm2p5, pm4p0, pm10p0;
		int16_t rh, temp, voc, nox;   /* SEN54 has no NOx -> nox=0x7FFF */

		err = sen5x_read_measured_values(&pm1p0, &pm2p5, &pm4p0, &pm10p0,
						 &rh, &temp, &voc, &nox);
		if (err) {
			printk("[READ ] SEN54 failed (err %d)\n", err);
			continue;
		}

		/* Scale: PM /10 ug/m3, RH /100 %, T /200 C, VOC /10 index */
		int32_t t_centi = (int32_t)temp / 2;   /* /200 *100 -> 0.01 C units */

		printk("\n\n--------------- SEN54 MEASUREMENT ------\n");
		printk("  PM1.0 : %u.%u ug/m3\n", pm1p0 / 10, pm1p0 % 10);
		printk("  PM2.5 : %u.%u ug/m3\n", pm2p5 / 10, pm2p5 % 10);
		printk("  PM4.0 : %u.%u ug/m3\n", pm4p0 / 10, pm4p0 % 10);
		printk("  PM10  : %u.%u ug/m3\n", pm10p0 / 10, pm10p0 % 10);
		printk("  Temp  : %d.%02d C\n",
		       t_centi / 100, (t_centi < 0 ? -t_centi : t_centi) % 100);
		printk("  Humid : %d.%02d %%\n", rh / 100, (rh < 0 ? -rh : rh) % 100);
		printk("  VOC   : %d.%d\n", voc / 10, (voc < 0 ? -voc : voc) % 10);

		/* --- SHT40 --- */
		if (device_is_ready(sht40) && sensor_sample_fetch(sht40) == 0) {
			struct sensor_value t, h;

			sensor_channel_get(sht40, SENSOR_CHAN_AMBIENT_TEMP, &t);
			sensor_channel_get(sht40, SENSOR_CHAN_HUMIDITY, &h);

			/* sensor_value: val1 = integer, val2 = millionths */
			printk("\n\n--------------- SHT40 MEASUREMENT ------\n");
			printk("  Temp  : %d.%02d C\n", t.val1, (t.val2 < 0 ? -t.val2 : t.val2) / 10000);
			printk("  Humid : %d.%02d %%\n", h.val1, (h.val2 < 0 ? -h.val2 : h.val2) / 10000);
		}
	}

	return 0;
}
