/*
 * Air-quality node: BMV080 (PM) + SHT40 (temp/RH) on i2c1.
 *
 * - one record per minute into flash (24 h ring), handed out over BLE
 * - buzzer sounds while PM2.5 >= 75 or PM10 >= 150 ug/m3
 *
 * PM in the wire frame comes from the BMV080, which is the only PM sensor left
 * on this build and reports float ug/m3 directly (no scaling). If it fails to
 * open the loop keeps running on SHT40 alone and records carry SENSOR_FAULT
 * rather than being dropped - the app and server tag bad data, never discard
 * it, because this is safety-management data.
 *
 * There is no calendar clock on this chip: RTC0/RTC1 are plain 32.768kHz
 * counters and the DK has no battery-backed RTC, so every record carries
 * SNTL_FLAG_RTC_UNSET and ts is device seconds since first boot.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

#include "bmv080.h"
#include "bmv080_defs.h"

#include "ble.h"
#include "frame.h"
#include "store.h"

static const struct i2c_dt_spec bmv080_spec = I2C_DT_SPEC_GET(DT_NODELABEL(bmv080));
/* SHT40 uses the mainline Zephyr sht4x driver via the sensor API */
static const struct device *const sht40 = DEVICE_DT_GET(DT_NODELABEL(sht40));
static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_NODELABEL(buzzer));
/* DK "Button 1" (P0.23) - opens the BLE advertising window on press. */
static const struct gpio_dt_spec adv_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback adv_btn_cb;

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

/* ---------------- buzzer ----------------
 * Passive buzzer: the PWM carrier IS the pitch, 50% duty = loudest. The DT
 * period is only a default - the melody sets its own per note.
 *
 * Alarm sound: the Mario coin jingle, B5 grace note into a held E6, then a
 * rest before it repeats. Played from the system workqueue rather than
 * k_msleep() because the BMV080 must be served at least once per second and
 * blocking the main loop to play a tune would miss that. */
static void tone_hz(uint32_t hz)
{
	uint32_t period;

	if (hz == 0) {
		pwm_set_pulse_dt(&buzzer, 0);
		return;
	}
	period = NSEC_PER_SEC / hz;
	pwm_set_dt(&buzzer, period, period / 2);
}

static const struct { uint32_t hz; uint16_t ms; } coin[] = {
	{  988,  80 },   /* B5 */
	{ 1319, 700 },   /* E6, held */
	{    0, 500 },   /* rest before the next repeat */
};

static bool coin_playing;
static int  coin_idx;

static void coin_step(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(coin_work, coin_step);

static void coin_step(struct k_work *w)
{
	ARG_UNUSED(w);

	if (!coin_playing) {
		tone_hz(0);
		return;
	}
	tone_hz(coin[coin_idx].hz);
	k_work_reschedule(&coin_work, K_MSEC(coin[coin_idx].ms));
	coin_idx = (coin_idx + 1) % ARRAY_SIZE(coin);
}

static void buzzer_set(bool on)
{
	static bool cur;

	if (on == cur) {
		return;
	}
	cur = on;
	coin_playing = on;
	if (on) {
		coin_idx = 0;
		k_work_reschedule(&coin_work, K_NO_WAIT);
	} else {
		k_work_cancel_delayable(&coin_work);
		tone_hz(0);
	}
}

/* ---------------- record storage off the main loop ----------------
 * An NVS write can erase a page (~85 ms) and occasionally garbage collect, and
 * the BMV080 has to be served at least once a second. So the minute record is
 * queued to a dedicated workqueue instead of being written inline. Its own
 * workqueue, not the system one, so it cannot stall the buzzer melody. */
#define STORE_WQ_STACK 2048

static K_THREAD_STACK_DEFINE(store_wq_stack, STORE_WQ_STACK);
static struct k_work_q store_wq;
static struct k_work store_work;
K_MSGQ_DEFINE(store_q, sizeof(struct sntl_record), 4, 4);

static void store_work_fn(struct k_work *w)
{
	struct sntl_record rec;

	ARG_UNUSED(w);

	while (k_msgq_get(&store_q, &rec, K_NO_WAIT) == 0) {
		int err = store_append(&rec);

		if (err) {
			printk("[STORE] append failed (%d)\n", err);
		} else {
			printk("[STORE] %us  PM2.5 %u  PM10 %u ug/m3\n",
			       rec.ts, rec.pm25, rec.pm10);
		}
	}
}

/* ---------------- measurement window ----------------
 * Accumulated at x10 ug/m3 so the per-minute average keeps a digit of
 * resolution before it is rounded to the integer the frame carries. */
static struct {
	uint32_t n;
	uint32_t pm25_sum;
	uint32_t pm10_sum;
	bool     obstructed;
} win;

#define AQ_PM10_HIGH   150.0f
#define AQ_PM25_HIGH   75.0f

/* BMV080 responsiveness knobs. The library defaults to HIGH_PRECISION over a
 * 10 s window, which averages a puff of dust away to nothing. FAST_RESPONSE
 * with a shorter window trades noise for reaction time. If readings get jumpy,
 * raise the window back toward 10 s or step up to BALANCED (2). */
#define BMV080_ALGORITHM       E_BMV080_MEASUREMENT_ALGORITHM_FAST_RESPONSE
#define BMV080_INTEGRATION_S   5.0f

/* Latest reading, x100 ug/m3, for the once-a-second status line. */
static int last_pm25, last_pm10;

static void bmv080_data_ready(bmv080_output_t out, void *param)
{
	ARG_UNUSED(param);

	last_pm25 = (int)(out.pm2_5_mass_concentration * 100.0f);
	last_pm10 = (int)(out.pm10_mass_concentration * 100.0f);

	if (out.is_obstructed) {
		printk("  [warn] sensor obstructed - clean the optical window\n");
		win.obstructed = true;
	}

	win.n++;
	win.pm25_sum += (uint32_t)(out.pm2_5_mass_concentration * 10.0f);
	win.pm10_sum += (uint32_t)(out.pm10_mass_concentration * 10.0f);

	bool high = (out.pm10_mass_concentration >= AQ_PM10_HIGH) ||
		    (out.pm2_5_mass_concentration >= AQ_PM25_HIGH);

	if (high) {
		printk("  *** ALARM: PM2.5>=75 or PM10>=150 ***\n");
	}
	buzzer_set(high);
}

/* One line a second: time, PM2.5, PM10 - plus temp/RH, which are free here and
 * are the only sign the SHT40 is still alive (they are not in the record). */
static void status_line(void)
{
	struct sensor_value t = { 0 }, h = { 0 };
	bool have_th = device_is_ready(sht40) && sensor_sample_fetch(sht40) == 0;

	if (have_th) {
		sensor_channel_get(sht40, SENSOR_CHAN_AMBIENT_TEMP, &t);
		sensor_channel_get(sht40, SENSOR_CHAN_HUMIDITY, &h);
	}

	printk("[%6us] PM2.5 %3d.%02d  PM10 %3d.%02d ug/m3", store_now(),
	       last_pm25 / 100, (last_pm25 < 0 ? -last_pm25 : last_pm25) % 100,
	       last_pm10 / 100, (last_pm10 < 0 ? -last_pm10 : last_pm10) % 100);

	if (have_th) {
		printk("   T %d.%01dC  RH %d.%01d%%",
		       t.val1, (t.val2 < 0 ? -t.val2 : t.val2) / 100000,
		       h.val1, (h.val2 < 0 ? -h.val2 : h.val2) / 100000);
	}
	printk("\n");
}

/* Turn the last minute into one record and queue it for flash. */
static void window_flush(void)
{
	struct sntl_record rec = { 0 };

	if (win.n > 0) {
		rec.pm25 = (uint16_t)(win.pm25_sum / win.n / 10);
		rec.pm10 = (uint16_t)(win.pm10_sum / win.n / 10);
	} else {
		rec.flags |= SNTL_FLAG_SENSOR_FAULT;   /* tag it, never drop it */
	}
	if (win.obstructed) {
		rec.flags |= SNTL_FLAG_SENSOR_FAULT;
	}
	/* No time source on this board yet - see the file header. */
	rec.flags |= SNTL_FLAG_RTC_UNSET;
	if (rec.pm10 >= SNTL_PM10_THRESHOLD) {
		rec.flags |= SNTL_FLAG_THRESHOLD_EXCEEDED;
	}
	/* DK runs off USB and has no battery gauge. Report full rather than a
	 * made up number; wire an ADC divider here for a real product. */
	rec.batt = 100;

	memset(&win, 0, sizeof(win));

	if (k_msgq_put(&store_q, &rec, K_NO_WAIT) != 0) {
		printk("[STORE] queue full, record dropped\n");
		return;
	}
	k_work_submit_to_queue(&store_wq, &store_work);
}

/* No debounce: a bounce just re-arms the same 20 s window, which is what a
 * second press would do anyway. */
static void adv_btn_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev); ARG_UNUSED(cb); ARG_UNUSED(pins);
	ble_advertise_window();
}

static int adv_btn_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&adv_btn)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&adv_btn, GPIO_INPUT);
	if (err) {
		return err;
	}
	err = gpio_pin_interrupt_configure_dt(&adv_btn, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		return err;
	}
	gpio_init_callback(&adv_btn_cb, adv_btn_pressed, BIT(adv_btn.pin));
	return gpio_add_callback(adv_btn.port, &adv_btn_cb);
}

int main(void)
{
	bmv080_handle_t bmv = NULL;
	int64_t next_tick;
	uint32_t sec = 0;
	int err;

	printk("\n");
	printk("========================================\n");
	printk("   Air Quality node: BMV080 + SHT40\n");
	printk("========================================\n");

	if (!device_is_ready(bmv080_spec.bus)) {
		printk("[ERROR] I2C bus not ready\n");
		return 0;
	}

	/* ---------------- buzzer ---------------- */
	if (pwm_is_ready_dt(&buzzer)) {
		printk("[READY] buzzer on P1.04\n");
		for (int i = 0; i < 2; i++) {   /* boot chime: one coin */
			tone_hz(coin[i].hz);
			k_msleep(coin[i].ms);
		}
		tone_hz(0);
	} else {
		printk("[WARN ] buzzer PWM not ready\n");
	}

	/* ---------------- storage ---------------- */
	err = store_init();
	if (err) {
		printk("[ERROR] store init failed (%d) - nothing will be recorded\n", err);
	} else {
		printk("[READY] store: next seq %u, oldest %u, t=%u s\n",
		       store_next_seq(), store_oldest_seq(), store_now());
	}

	k_work_queue_start(&store_wq, store_wq_stack, K_THREAD_STACK_SIZEOF(store_wq_stack),
			   K_PRIO_PREEMPT(8), NULL);
	k_work_init(&store_work, store_work_fn);

	/* ---------------- BLE ---------------- */
	err = ble_start();
	if (err) {
		printk("[ERROR] BLE start failed (%d)\n", err);
	}

	err = adv_btn_init();
	if (err) {
		printk("[WARN ] Button 1 not available (%d) - no way to advertise\n", err);
	}

	/* ---------------- BMV080 ---------------- */
	bmv080_status_code_t bst = bmv080_open(&bmv, (bmv080_sercom_handle_t)&bmv080_spec,
					       bmv080_read, bmv080_write, bmv080_delay);
	if (bst != E_BMV080_OK) {
		printk("[WARN ] BMV080 open failed (%d) - check wiring/PS pin, running SHT40 only\n",
		       (int)bst);
		bmv = NULL;
	} else {
		char id[13] = {0};
		bmv080_measurement_algorithm_t algo = BMV080_ALGORITHM;
		float integration_s = BMV080_INTEGRATION_S;

		if (bmv080_get_sensor_id(bmv, id) == E_BMV080_OK) {
			printk("[READY] BMV080 (id: %s)\n", id);
		}

		/* Both must be set before start_continuous_measurement() or they
		 * are ignored. On rejection the library keeps its default, so
		 * the warning below is the only sign it didn't take. */
		bst = bmv080_set_parameter(bmv, "measurement_algorithm", &algo);
		if (bst != E_BMV080_OK) {
			printk("[WARN ] measurement_algorithm rejected (%d)\n", (int)bst);
		}
		bst = bmv080_set_parameter(bmv, "integration_time", &integration_s);
		if (bst != E_BMV080_OK) {
			printk("[WARN ] integration_time rejected (%d) - still 10 s\n",
			       (int)bst);
		}

		if (bmv080_start_continuous_measurement(bmv) == E_BMV080_OK) {
			printk("[START] BMV080 measuring (fast response, %d s window)\n",
			       (int)BMV080_INTEGRATION_S);
		}
	}

	/* ---------------- SHT40 (mainline sensor driver) ---------------- */
	if (device_is_ready(sht40)) {
		printk("[READY] SHT40\n");
	} else {
		printk("[WARN ] SHT40 not ready - check wiring, skipping it\n");
	}

	/* Absolute 1 s cadence rather than k_msleep(1000) at the end: printk and
	 * the I2C reads take time, and letting that push the period past 1 s
	 * would starve bmv080_serve_interrupt(). */
	next_tick = k_uptime_get() + 1000;

	while (1) {
		if (bmv != NULL) {
			bmv080_serve_interrupt(bmv, bmv080_data_ready, NULL);
		}

		status_line();

		if (++sec >= STORE_PERIOD_S) {
			sec = 0;
			window_flush();
		}

		int64_t delay = next_tick - k_uptime_get();

		if (delay > 0) {
			k_msleep((int32_t)delay);
		}
		next_tick += 1000;
	}

	return 0;
}
