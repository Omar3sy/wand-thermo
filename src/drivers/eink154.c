/*
 * Eigener Zephyr-Treiber fuer das Microtech MTE0154A03B-V1 (1.54" EPD),
 * basierend auf dem von Microtech gelieferten Referenzcode (STM32-HAL).
 * Befehlssatz NICHT ST7305-kompatibel - eigenstaendiger Treiber, kein
 * MIPI-DBI-Bus.
 */

#define DT_DRV_COMPAT microtech_eink154

#include <zephyr/drivers/display.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(eink154, CONFIG_DISPLAY_LOG_LEVEL);

struct eink154_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec dc_gpio;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec busy_gpio;
	uint16_t width;
	uint16_t height;
};

struct eink154_data {
	/* Framebuffer, 1 bpp, MSB first je Zeile - Groesse haengt von
	 * cfg->width * cfg->height ab, wird in eink154_init() dynamisch
	 * alloziert statt wie beim Referenzcode fest auf 200x200 anzunehmen. */
	uint8_t *framebuf;
	size_t framebuf_size;
};

/* --- Low-Level SPI --- */

static int eink154_send_cmd(const struct device *dev, uint8_t cmd)
{
	const struct eink154_config *cfg = dev->config;
	struct spi_buf buf = { .buf = &cmd, .len = 1 };
	struct spi_buf_set set = { .buffers = &buf, .count = 1 };

	gpio_pin_set_dt(&cfg->dc_gpio, 0); /* Command-Modus */
	int ret = spi_write_dt(&cfg->spi, &set);
	if (ret != 0) {
		LOG_ERR("eink154: spi_write_dt(cmd 0x%02x) failed: %d", cmd, ret);
	}
	return ret;
}

static int eink154_send_data_byte(const struct device *dev, uint8_t data)
{
	const struct eink154_config *cfg = dev->config;
	struct spi_buf buf = { .buf = &data, .len = 1 };
	struct spi_buf_set set = { .buffers = &buf, .count = 1 };

	gpio_pin_set_dt(&cfg->dc_gpio, 1); /* Data-Modus */
	int ret = spi_write_dt(&cfg->spi, &set);
	if (ret != 0) {
		LOG_ERR("eink154: spi_write_dt(data 0x%02x) failed: %d", data, ret);
	}
	return ret;
}

static int eink154_send_data_buf(const struct device *dev, const uint8_t *data, size_t len)
{
	const struct eink154_config *cfg = dev->config;
	struct spi_buf buf = { .buf = (void *)data, .len = len };
	struct spi_buf_set set = { .buffers = &buf, .count = 1 };

	gpio_pin_set_dt(&cfg->dc_gpio, 1);
	int ret = spi_write_dt(&cfg->spi, &set);
	if (ret != 0) {
		LOG_ERR("eink154: spi_write_dt(buf, %zu bytes) failed: %d", len, ret);
	}
	return ret;
}

/* R71H-Statusregister-Idee wieder entfernt: dieses Breakout-Board hat
 * nachweislich keinen SDO/MISO-Pin (nur BUSY, RST, DC, CS, CLK, DIN, GND,
 * VCC) - ein Rueckkanal zum Panel existiert hardwareseitig gar nicht,
 * jeder Lese-Versuch liest nur einen floatenden MCU-Pin aus. */

/* --- Busy / Reset, direkte Entsprechung zu ReadBusy() / HardWare_Reset() --- */

/* Verifiziert per Multimeter/Blinktest: BUSY-Polaritaet und Verdrahtung sind
 * korrekt (busy = Pegel 0, wie im Referenzcode). Das Timeout ist ein
 * dauerhaftes Sicherheitsnetz gegen ein wirklich haengendes Panel, nicht
 * mehr zur Fehlersuche - 3s war zu knapp fuer einen echten Refresh-Zyklus
 * (Power On + Display Refresh + Power Off koennen zusammen laenger dauern),
 * das loeste frueher staendig falschen Alarm aus. */
static void eink154_wait_busy(const struct device *dev)
{
	const struct eink154_config *cfg = dev->config;
	int initial = gpio_pin_get_dt(&cfg->busy_gpio);
	int waited_ms = 0;
	const int timeout_ms = 15000;

	while (gpio_pin_get_dt(&cfg->busy_gpio) == 0 && waited_ms < timeout_ms) {
		k_msleep(5);
		waited_ms += 5;
	}

	int final = gpio_pin_get_dt(&cfg->busy_gpio);

	if (waited_ms >= timeout_ms) {
		LOG_WRN("eink154: busy wait timed out after %d ms (pin stayed low)", waited_ms);
	} else {
		LOG_INF("eink154: busy wait: initial=%d final=%d waited=%d ms", initial, final, waited_ms);
	}
}

static void eink154_hw_reset(const struct device *dev)
{
	const struct eink154_config *cfg = dev->config;

	/* BUG FIX: the sequence copied from the reference code ended with the
	 * reset line asserted (logical 1 = held in reset, since reset-gpios is
	 * GPIO_ACTIVE_LOW), leaving the panel permanently in reset - it never
	 * ran at all, explaining "no error, but nothing on the panel". Ends
	 * released (logical 0) now, matching assert/release/assert/release. */
	k_msleep(10);
	gpio_pin_set_dt(&cfg->reset_gpio, 1); /* assert */
	k_msleep(10);
	gpio_pin_set_dt(&cfg->reset_gpio, 0); /* release */
	k_msleep(10);
	gpio_pin_set_dt(&cfg->reset_gpio, 1); /* assert again */
	k_msleep(10);
	gpio_pin_set_dt(&cfg->reset_gpio, 0); /* release - panel runs from here */
	k_msleep(20);
}

/* IC-Init, jetzt wieder 1:1 nach Microtechs eigenem Referenzcode (von
 * Monica/Microtech-Support am 2026-10-09 bestaetigt und nachgereicht:
 * "Our product does not have OTP programmed ... please use the external
 * code, which is the reference program we provided"). Die vorherige
 * Kurzform nach Datenblatt-Abschnitt 11.1 ("LUT from OTP Operation Flow")
 * war falsch fuer dieses konkrete Teil - das Teil hat laut Microtech KEIN
 * OTP programmiert, der Abschnitt gilt hier nicht. Alle Register-Werte
 * unten sind wortgetreu aus Microtechs IC_Init()/lut_find() uebernommen,
 * keine eigenen Vermutungen mehr. */
static void eink154_ic_init(const struct device *dev)
{
	eink154_send_cmd(dev, 0x4D);
	eink154_send_data_byte(dev, 0x55);
	eink154_send_cmd(dev, 0xF3);
	eink154_send_data_byte(dev, 0x0A);
	eink154_send_cmd(dev, 0xAA);
	eink154_send_data_byte(dev, 0xB7); /* PASSWORD */

	eink154_send_cmd(dev, 0x00); /* panel setting */
	eink154_send_data_byte(dev, 0x3F); /* 176*264,LUT from,kw mode,scan up,shift right,booster on */
	eink154_send_data_byte(dev, 0x0C);

	eink154_send_cmd(dev, 0x01); /* power setting */
	eink154_send_data_byte(dev, 0x03); /* internal DC/DC function */
	eink154_send_data_byte(dev, 0x00); /* VCOM voltage level */
	eink154_send_data_byte(dev, 0x30); /* VDH=15v */
	eink154_send_data_byte(dev, 0x30); /* VDL=-15v */
	eink154_send_data_byte(dev, 0x00); /* VSHR 6.2V */

	eink154_send_cmd(dev, 0x06); /* booster start */
	eink154_send_data_byte(dev, 0xC7);
	eink154_send_data_byte(dev, 0xC7);
	eink154_send_data_byte(dev, 0x2F);
	eink154_send_data_byte(dev, 0xA5);
	eink154_send_data_byte(dev, 0x33);

	eink154_send_cmd(dev, 0x61); /* resolution setting */
	eink154_send_data_byte(dev, 0xC8); /* hres */
	eink154_send_data_byte(dev, 0xC8); /* vres */

	eink154_send_cmd(dev, 0x30); /* PLL control */
	eink154_send_data_byte(dev, 0x13);

	eink154_send_cmd(dev, 0x60); /* TCON */
	eink154_send_data_byte(dev, 0x00);

	eink154_send_cmd(dev, 0x82); /* VCOM_DC */
	eink154_send_data_byte(dev, 0x05);

	eink154_send_cmd(dev, 0x03);
	eink154_send_data_byte(dev, 0x00);

	eink154_send_cmd(dev, 0x50);
	eink154_send_data_byte(dev, 0x97);

	eink154_send_cmd(dev, 0x04);
	eink154_wait_busy(dev);
}

/* Wellenform-LUT ("GC" - full refresh), wortgetreu aus Microtechs
 * Referenzcode. 5 Bloecke a 42 Byte: VCOM, LUTKK (schwarz->schwarz),
 * LUTWK (weiss->schwarz), LUTKW (schwarz->weiss), LUTWW (weiss->weiss). */
static const uint8_t eink154_lut_gc[210] = {
	/* VCOM */
	0x01, 0x1E, 0x1E, 0x00, 0x00, 0x01, 0x00,
	0x01, 0x1E, 0x1E, 0x1E, 0x00, 0x01, 0x01,
	0x01, 0x14, 0x14, 0x05, 0x05, 0x01, 0x02,
	0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	/* LUT_WW */
	0x01, 0x5E, 0x1E, 0x00, 0x00, 0x01, 0x00,
	0x01, 0x9E, 0x5E, 0x1E, 0x00, 0x01, 0x01,
	0x01, 0x94, 0x14, 0x85, 0x05, 0x01, 0x02,
	0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	/* LUT_BW */
	0x01, 0x5E, 0x1E, 0x00, 0x00, 0x01, 0x00,
	0x01, 0x9E, 0x5E, 0x1E, 0x00, 0x01, 0x01,
	0x01, 0x94, 0x14, 0x85, 0x05, 0x01, 0x02,
	0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	/* LUT_WB */
	0x01, 0x1E, 0x9E, 0x00, 0x00, 0x01, 0x00,
	0x01, 0x1E, 0x5E, 0x9E, 0x00, 0x01, 0x01,
	0x01, 0x14, 0x54, 0x05, 0x45, 0x01, 0x02,
	0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	/* LUT_BB */
	0x01, 0x1E, 0x9E, 0x00, 0x00, 0x01, 0x00,
	0x01, 0x1E, 0x5E, 0x9E, 0x00, 0x01, 0x01,
	0x01, 0x14, 0x54, 0x05, 0x45, 0x01, 0x02,
	0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
};

static void eink154_load_lut(const struct device *dev, const uint8_t *lut)
{
	eink154_send_cmd(dev, 0x20); /* VCOM */
	eink154_send_data_buf(dev, &lut[0], 42);
	eink154_send_cmd(dev, 0x21); /* LUTKK */
	eink154_send_data_buf(dev, &lut[42], 42);
	eink154_send_cmd(dev, 0x22); /* LUTWK */
	eink154_send_data_buf(dev, &lut[84], 42);
	eink154_send_cmd(dev, 0x23); /* LUTKW */
	eink154_send_data_buf(dev, &lut[126], 42);
	eink154_send_cmd(dev, 0x24); /* LUTWW */
	eink154_send_data_buf(dev, &lut[168], 42);
}

static void eink154_display_update(const struct device *dev)
{
	eink154_send_cmd(dev, 0x04); /* Power On */
	eink154_wait_busy(dev);
	eink154_send_cmd(dev, 0x12); /* Display Refresh */
	eink154_wait_busy(dev);
	eink154_send_cmd(dev, 0x02); /* Power Off */
	eink154_wait_busy(dev);
}

/* --- Zephyr display-API --- */

static int eink154_write(const struct device *dev, const uint16_t x, const uint16_t y,
			 const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct eink154_config *cfg = dev->config;
	struct eink154_data *data = dev->data;

	/* Vereinfachend: diese erste Version erwartet immer einen Full-Frame-Write
	 * (x=0, y=0, desc->width=cfg->width, desc->height=cfg->height), passend zu
	 * eurem bisherigen DisplayManager-Nutzungsmuster (ganze Zeilen neu schreiben).
	 * Partial-Update ist beim full-refresh-Referenzcode ohnehin nicht vorgesehen. */
	if (x != 0 || y != 0 || desc->width != cfg->width || desc->height != cfg->height) {
		LOG_ERR("eink154: nur Full-Frame-Writes unterstuetzt in dieser Version");
		return -ENOTSUP;
	}

	memcpy(data->framebuf, buf, data->framebuf_size);

	/* Nach Microtechs dis_first(): "alte" Daten (konstant 0xFF) ueber 0x10,
	 * dann das neue Bild ueber 0x13. Die im Referenzcode vorhandene
	 * Invertierung (~new_dat[i]) wurde entfernt - erster Testlauf zeigte
	 * schwarzen Hintergrund mit weissem Text (genau vertauscht), die
	 * Invertierung war also fuer unsere Framebuffer-Konvention (bit=0 =
	 * schwarz, siehe font_render.c) falsch herum. Ohne ~ passt es direkt:
	 * weisser Hintergrund (Bit 1), schwarzer Text (Bit 0). */
	eink154_send_cmd(dev, 0x10);
	for (size_t i = 0; i < data->framebuf_size; i++) {
		eink154_send_data_byte(dev, 0xFF);
	}

	eink154_send_cmd(dev, 0x13);
	eink154_send_data_buf(dev, data->framebuf, data->framebuf_size);

	eink154_display_update(dev);

	return 0;
}

static int eink154_blanking_on(const struct device *dev)
{
	/* E-Paper hat kein echtes Blanking - nichts zu tun, Bild bleibt stehen. */
	return 0;
}

static int eink154_blanking_off(const struct device *dev)
{
	return 0;
}

static void eink154_get_capabilities(const struct device *dev,
				     struct display_capabilities *caps)
{
	const struct eink154_config *cfg = dev->config;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = cfg->width;
	caps->y_resolution = cfg->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO01;
	caps->current_pixel_format = PIXEL_FORMAT_MONO01;
	caps->screen_info = SCREEN_INFO_MONO_VTILED; /* je nach Bit-Reihenfolge ggf. anpassen */
}

static int eink154_set_pixel_format(const struct device *dev,
				    const enum display_pixel_format fmt)
{
	return (fmt == PIXEL_FORMAT_MONO01) ? 0 : -ENOTSUP;
}

static int eink154_init(const struct device *dev)
{
	const struct eink154_config *cfg = dev->config;
	struct eink154_data *data = dev->data;

	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("eink154: SPI-Bus nicht bereit");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&cfg->dc_gpio) || !gpio_is_ready_dt(&cfg->reset_gpio) ||
	    !gpio_is_ready_dt(&cfg->busy_gpio)) {
		LOG_ERR("eink154: GPIOs nicht bereit");
		return -ENODEV;
	}

	data->framebuf_size = (size_t)cfg->width * cfg->height / 8;
	data->framebuf = k_malloc(data->framebuf_size);
	if (data->framebuf == NULL) {
		LOG_ERR("eink154: kein Speicher fuer Framebuffer (%zu Byte)", data->framebuf_size);
		return -ENOMEM;
	}
	memset(data->framebuf, 0xFF, data->framebuf_size); /* weiss initialisieren */

	gpio_pin_configure_dt(&cfg->dc_gpio, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&cfg->busy_gpio, GPIO_INPUT);

	eink154_hw_reset(dev);
	eink154_wait_busy(dev);
	eink154_ic_init(dev);
	eink154_load_lut(dev, eink154_lut_gc);

	LOG_INF("eink154: initialisiert, %ux%u", cfg->width, cfg->height);
	return 0;
}

static DEVICE_API(display, eink154_api) = {
	.blanking_on = eink154_blanking_on,
	.blanking_off = eink154_blanking_off,
	.write = eink154_write,
	.get_capabilities = eink154_get_capabilities,
	.set_pixel_format = eink154_set_pixel_format,
};

#define EINK154_INIT(n)                                                                      \
	static struct eink154_data eink154_data_##n;                                          \
	static const struct eink154_config eink154_config_##n = {                             \
		.spi = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8), 0),                            \
		.dc_gpio = GPIO_DT_SPEC_INST_GET(n, dc_gpios),                                 \
		.reset_gpio = GPIO_DT_SPEC_INST_GET(n, reset_gpios),                           \
		.busy_gpio = GPIO_DT_SPEC_INST_GET(n, busy_gpios),                             \
		.width = DT_INST_PROP(n, width),                                               \
		.height = DT_INST_PROP(n, height),                                             \
	};                                                                                      \
	DEVICE_DT_INST_DEFINE(n, eink154_init, NULL, &eink154_data_##n, &eink154_config_##n,   \
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &eink154_api);

DT_INST_FOREACH_STATUS_OKAY(EINK154_INIT)
