// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Copyright (c) 2025, Danila Tikhonov <danila@jiaxyga.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/irqflags.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct tianma_nt37705 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data *supplies;

	/*
	 * Optional Pixelworks Iris 5 between the DSI host and the panel
	 * (realme GT Neo5). It is kept in analog bypass, where it only needs
	 * power, a reset pulse and one "power down sys" one-wire command.
	 */
	struct regulator *iris_vdd;
	struct gpio_desc *iris_reset_gpio;
	struct gpio_desc *iris_wakeup_gpio;

	/* PWM dimming mode last sent: 0 low, 1 high, -1 unknown (after reset) */
	int pwm_mode;

	/* for the mode being enabled */
	struct drm_connector *connector;
};

/*
 * What the refresh rates change in the init sequence, from the
 * timing@fhd_sdc_{144,120,90,60} nodes of the vendor device tree.
 */
struct tianma_nt37705_mode {
	struct drm_display_mode mode;
	u8 fre;		/* 2Fh before sleep out */
	u8 ba;		/* BAh at offsets A8h and B0h */
	u8 fre_on;	/* 2Fh before display on */
	bool rate_60;	/* no TE before sleep out, 6Dh 01, BEh not sent */
};

#define TIANMA_NT37705_MODE(hbp, rate) {				\
	.clock = (1240 + 64 + 8 + (hbp)) * (2772 + 2 + 2 + 8) * (rate) / 1000, \
	.hdisplay = 1240,						\
	.hsync_start = 1240 + 64,					\
	.hsync_end = 1240 + 64 + 8,					\
	.htotal = 1240 + 64 + 8 + (hbp),				\
	.vdisplay = 2772,						\
	.vsync_start = 2772 + 2,					\
	.vsync_end = 2772 + 2 + 2,					\
	.vtotal = 2772 + 2 + 2 + 8,					\
	.width_mm = 70,							\
	.height_mm = 156,						\
	.type = DRM_MODE_TYPE_DRIVER,					\
}

/* The first one is the preferred mode */
static const struct tianma_nt37705_mode tianma_nt37705_modes[] = {
	{
		.mode = TIANMA_NT37705_MODE(48, 144),
		.fre = 0x02, .ba = 0x22, .fre_on = 0x30,
	}, {
		.mode = TIANMA_NT37705_MODE(50, 120),
		.fre = 0x00, .ba = 0x00, .fre_on = 0x30,
	}, {
		.mode = TIANMA_NT37705_MODE(49, 90),
		.fre = 0x01, .ba = 0x11, .fre_on = 0x31,
	}, {
		.mode = TIANMA_NT37705_MODE(48, 60),
		.fre = 0x00, .ba = 0x00, .fre_on = 0x30, .rate_60 = true,
	},
};

static const struct tianma_nt37705_mode *
tianma_nt37705_current_mode(struct tianma_nt37705 *ctx)
{
	const struct drm_display_mode *mode;
	int i;

	if (ctx->connector && ctx->connector->state &&
	    ctx->connector->state->crtc) {
		mode = &ctx->connector->state->crtc->state->mode;
		for (i = 0; i < ARRAY_SIZE(tianma_nt37705_modes); i++)
			if (drm_mode_match(mode, &tianma_nt37705_modes[i].mode,
					   DRM_MODE_MATCH_TIMINGS |
					   DRM_MODE_MATCH_CLOCK))
				return &tianma_nt37705_modes[i];
	}

	return &tianma_nt37705_modes[0];
}

/* Enabled in this order, 1 ms apart (downstream qcom,supply-post-on-sleep) */
static const struct regulator_bulk_data tianma_nt37705_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vdd" },
	{ .supply = "vci" },
};

static inline
struct tianma_nt37705 *to_tianma_nt37705(struct drm_panel *panel)
{
	return container_of(panel, struct tianma_nt37705, panel);
}

static int tianma_nt37705_power_on(struct tianma_nt37705 *ctx)
{
	int i, ret;

	if (ctx->iris_vdd) {
		ret = regulator_enable(ctx->iris_vdd);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(tianma_nt37705_supplies); i++) {
		ret = regulator_enable(ctx->supplies[i].consumer);
		if (ret)
			goto err;
		usleep_range(1000, 1500);
	}

	return 0;

err:
	while (--i >= 0)
		regulator_disable(ctx->supplies[i].consumer);
	if (ctx->iris_vdd)
		regulator_disable(ctx->iris_vdd);
	return ret;
}

static void tianma_nt37705_power_off(struct tianma_nt37705 *ctx)
{
	int i;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	gpiod_set_value_cansleep(ctx->iris_reset_gpio, 1);

	if (ctx->iris_vdd)
		regulator_disable(ctx->iris_vdd);

	for (i = ARRAY_SIZE(tianma_nt37705_supplies) - 1; i >= 0; i--)
		regulator_disable(ctx->supplies[i].consumer);
}

/* Downstream iris_reset_chip(): low for 1 ms, then 2 ms before use */
static void tianma_nt37705_iris_reset(struct tianma_nt37705 *ctx)
{
	if (!ctx->iris_reset_gpio)
		return;

	gpiod_set_value_cansleep(ctx->iris_reset_gpio, 1);
	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(ctx->iris_reset_gpio, 0);
	usleep_range(2000, 2100);
}

/*
 * Downstream iris_send_one_wired_cmd(IRIS_POWER_DOWN_SYS): four pulses of
 * 56 us high / 56 us low on the wakeup GPIO. After a reset the chip is in
 * analog bypass; this powers down its digital part and leaves the DSI
 * lanes passed through to the panel ("Enter abyp done" downstream).
 */
#define IRIS_ONE_WIRE_PULSE_US		56
#define IRIS_ONE_WIRE_CMD_DELAY_US	10
#define IRIS_ONE_WIRE_START_END_US	227
#define IRIS_POWER_DOWN_SYS_PULSES	4

static void tianma_nt37705_iris_bypass(struct tianma_nt37705 *ctx)
{
	unsigned long flags;
	int i;

	if (!ctx->iris_wakeup_gpio)
		return;

	local_irq_save(flags);
	for (i = 0; i < IRIS_POWER_DOWN_SYS_PULSES; i++) {
		gpiod_set_value(ctx->iris_wakeup_gpio, 1);
		udelay(IRIS_ONE_WIRE_PULSE_US);
		gpiod_set_value(ctx->iris_wakeup_gpio, 0);
		udelay(IRIS_ONE_WIRE_PULSE_US);
	}
	udelay(IRIS_ONE_WIRE_CMD_DELAY_US);
	local_irq_restore(flags);

	udelay(IRIS_ONE_WIRE_START_END_US);
}

static void tianma_nt37705_reset(struct tianma_nt37705 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
}

static int tianma_nt37705_on(struct tianma_nt37705 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	const struct tianma_nt37705_mode *m = tianma_nt37705_current_mode(ctx);

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* Optimize edge green */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4,
				     0xc0, 0x80, 0x80, 0x80, 0x80, 0x50, 0x80,
				     0x80, 0x80, 0x80, 0x80, 0x50, 0x80);
	/* Vsync Hsync output */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x49, 0x4c);
	/* AVDD=7.9 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x4e);
	/* HBM L0 glows green */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x34);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x83);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x12);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x41);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x17, 0x10);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x04d7);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x0ad3);
	/* Frame rate */
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0x2f, m->fre);
	/* TE ON */
	if (!m->rate_60)
		mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx,
					       MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_set_tear_scanline_multi(&dsi_ctx, 0x0000);
	/* Nor 51 */
	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, 0x0000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x04);
	/* AOD 51 */
	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, 0x0000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03, 0x03);
	/* 10bit DSC */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91,
				     0xab, 0x28, 0x00, 0x0c, 0xf2, 0x00, 0x02,
				     0x85, 0x01, 0x0f, 0x00, 0x08, 0x0a, 0xe9,
				     0x07, 0x5f, 0x10, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x24);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb,
				     0x00, 0x03, 0x04, 0x55, 0x77, 0x77, 0x77,
				     0x99, 0x9d, 0x00, 0x00, 0x06, 0x88, 0x9a,
				     0xbb, 0xbc, 0xde, 0xef, 0xf0, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf5, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0xfc, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf6, 0x70, 0x70, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf6, 0x60);
	/* OSC start */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x11);
	/* OSC2 145Mhz */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x01, 0x8e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2d);
	/* OSC1 121.9Mhz */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x00, 0xfc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x0f);
	/* OSC end */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x83);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x12);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x41);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf9, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5a, 0x00);
	/* AOD remove black frame */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2,
				     0x00, 0x00, 0x00, 0x00, 0x15);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2, 0x01, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2,
				     0x02, 0x00, 0x10, 0x05, 0x88);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0x80, 0x00, 0x00, 0x10, 0x00, 0x53);
	/* AOD off splash */    
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2,
				     0x02, 0x00, 0x10, 0x05, 0x82);
	/* 11H sleep out */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x11, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 128);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0x2f, m->fre);
	mipi_dsi_usleep_range(&dsi_ctx, 17000, 18000);
	if (!m->rate_60)
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x47);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0xa8);
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0xba, m->ba, m->ba);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0xb0);
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0xba, m->ba, m->ba, 0x00, 0x00);
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0x2f, m->fre_on);
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0x6d, m->rate_60 ? 0x01 : 0x00);
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0x35, 0x00, 0x00, 0x00,
				     m->rate_60 ? 0x01 : 0x00);
	/*
	 * The vendor on-commands of every refresh rate end here. The C3h
	 * group that follows display on in qcom,mdss-dsi-timing-switch-command
	 * belongs to a switch between rates only: sent after display on, the
	 * panel stopped answering after minutes of use (no TE, kickoff
	 * timeouts, DSI command DMA timeouts).
	 */
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int tianma_nt37705_off(struct tianma_nt37705 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	/* Display Off & Sleep In */
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_usleep_range(&dsi_ctx, 10000, 11000);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int tianma_nt37705_prepare(struct drm_panel *panel)
{
	struct tianma_nt37705 *ctx = to_tianma_nt37705(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = tianma_nt37705_power_on(ctx);
	if (ret < 0)
		return ret;

	/* The reset drops the PWM mode, resend it with the next brightness */
	ctx->pwm_mode = -1;

	/* Same order as downstream dsi_panel_reset(): Iris, then the panel */
	tianma_nt37705_iris_reset(ctx);
	tianma_nt37705_reset(ctx);
	tianma_nt37705_iris_bypass(ctx);

	ret = tianma_nt37705_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		goto err_power_off;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS: %d\n", ret);
		goto err_power_off;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		goto err_power_off;
	}

	msleep(28);

	return 0;

err_power_off:
	tianma_nt37705_power_off(ctx);
	return ret;
}

static int tianma_nt37705_unprepare(struct drm_panel *panel)
{
	struct tianma_nt37705 *ctx = to_tianma_nt37705(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = tianma_nt37705_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	tianma_nt37705_power_off(ctx);

	return 0;
}

static int tianma_nt37705_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	struct tianma_nt37705 *ctx = to_tianma_nt37705(panel);
	struct drm_display_mode *mode;
	int i;

	ctx->connector = connector;

	for (i = 0; i < ARRAY_SIZE(tianma_nt37705_modes); i++) {
		mode = drm_mode_duplicate(connector->dev,
					  &tianma_nt37705_modes[i].mode);
		if (!mode)
			return i;

		drm_mode_set_name(mode);
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = tianma_nt37705_modes[0].mode.width_mm;
	connector->display_info.height_mm = tianma_nt37705_modes[0].mode.height_mm;

	return i;
}

static const struct drm_panel_funcs tianma_nt37705_panel_funcs = {
	.prepare = tianma_nt37705_prepare,
	.unprepare = tianma_nt37705_unprepare,
	.get_modes = tianma_nt37705_get_modes,
};

/*
 * The panel has two PWM dimming modes. Above this level it must be put in
 * the "high" mode, otherwise blacks are visibly raised (measured on senna:
 * fine at 1600, washed out at 2048). Threshold and register sets are
 * oplus,pwm-switch-backlight-threshold and qcom,mdss-dsi-pwm-switch-{high,low}
 * -command from the vendor device tree; both sets are identical apart from
 * the B2 values.
 */
#define TIANMA_NT37705_PWM_SWITCH_LEVEL	1603

static void tianma_nt37705_pwm_switch(struct mipi_dsi_multi_context *dsi_ctx,
				      bool high)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6f, 0x11);
	if (high)
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0x02, 0x03, 0x42);
	else
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0x0f, 0x17, 0x4e);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb3,
				     0x00, 0x08, 0x00, 0xb4, 0x00, 0xb4, 0x01,
				     0x5f, 0x01, 0x5f, 0x02, 0x20, 0x02, 0x20,
				     0x03, 0x7c, 0x03, 0x7c, 0x05, 0x30, 0x05,
				     0x30, 0x06, 0x43, 0x06, 0x43, 0x06, 0x44,
				     0x06, 0x44, 0x09, 0x0b, 0x09, 0x0b, 0x0d,
				     0xbb, 0x0d, 0xbb, 0x0f, 0xff, 0x0f, 0xff,
				     0x0f, 0xff);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb4,
				     0x00, 0x00, 0x00, 0x80, 0x00, 0x96, 0x00,
				     0x96, 0x00, 0xc0, 0x00, 0xc0, 0x01, 0x26,
				     0x01, 0x26, 0x02, 0x8e, 0x02, 0x8e, 0x05,
				     0x8e, 0x05, 0x8e, 0x08, 0x26, 0x08, 0x26,
				     0x09, 0x40, 0x09, 0x40, 0x09, 0x40, 0x09,
				     0x40, 0x09, 0x40, 0x09, 0x40, 0x09, 0x40,
				     0x09, 0x40, 0x09, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6f, 0x2e);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb4,
				     0x00, 0x00, 0x00, 0xe2, 0x00, 0xff, 0x00,
				     0xff, 0x01, 0x3a, 0x01, 0x3a, 0x01, 0xce,
				     0x01, 0xce, 0x03, 0xaa, 0x03, 0xaa, 0x07,
				     0x9c, 0x07, 0x9c, 0x0b, 0x20, 0x0b, 0x20,
				     0x0c, 0x56, 0x0c, 0x56, 0x0c, 0x56, 0x0c,
				     0x56, 0x0c, 0x56, 0x0c, 0x56, 0x0c, 0x56,
				     0x0c, 0x56, 0x0c, 0x56);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6f, 0x5c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb4,
				     0x00, 0x00, 0x00, 0x78, 0x00, 0x8c, 0x00,
				     0x8c, 0x00, 0xb4, 0x00, 0xb4, 0x01, 0x26,
				     0x01, 0x26, 0x02, 0x8e, 0x02, 0x8e, 0x05,
				     0x8e, 0x05, 0x8e, 0x08, 0x26, 0x08, 0x26,
				     0x09, 0x40, 0x09, 0x40, 0x09, 0x40, 0x09,
				     0x40, 0x09, 0x40, 0x09, 0x40, 0x09, 0x40,
				     0x09, 0x40, 0x09, 0x40);
}

static int tianma_nt37705_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct tianma_nt37705 *ctx = mipi_dsi_get_drvdata(dsi);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	u16 brightness = backlight_get_brightness(bl);
	int mode = brightness > TIANMA_NT37705_PWM_SWITCH_LEVEL;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	/*
	 * Never leave the panel in the low mode with a level above the
	 * threshold, not even between two writes: that shows as a washed out
	 * flash. Going down, lower the level first; going up, switch the mode
	 * first. Turning the backlight off keeps the current mode, the panel
	 * is about to be powered off and prepare() resends it.
	 */
	if (brightness && ctx->pwm_mode != mode && mode) {
		/* Only on a change: the set is ~200 bytes */
		tianma_nt37705_pwm_switch(&dsi_ctx, mode);
		if (dsi_ctx.accum_err)
			return dsi_ctx.accum_err;
		ctx->pwm_mode = mode;
	}

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	if (brightness && ctx->pwm_mode != mode && !mode) {
		tianma_nt37705_pwm_switch(&dsi_ctx, mode);
		if (dsi_ctx.accum_err)
			return dsi_ctx.accum_err;
		ctx->pwm_mode = mode;
	}

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int tianma_nt37705_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops tianma_nt37705_bl_ops = {
	.update_status = tianma_nt37705_bl_update_status,
	.get_brightness = tianma_nt37705_bl_get_brightness,
};

static struct backlight_device *
tianma_nt37705_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 2048,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &tianma_nt37705_bl_ops, &props);
}

static int tianma_nt37705_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct tianma_nt37705 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct tianma_nt37705, panel,
				   &tianma_nt37705_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(&dsi->dev,
					    ARRAY_SIZE(tianma_nt37705_supplies),
					    tianma_nt37705_supplies,
					    &ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->iris_vdd = devm_regulator_get_optional(dev, "iris-vdd");
	if (IS_ERR(ctx->iris_vdd)) {
		if (PTR_ERR(ctx->iris_vdd) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(ctx->iris_vdd),
					     "Failed to get iris-vdd\n");
		ctx->iris_vdd = NULL;
	}

	/* Left running by the bootloader: request deasserted / idle */
	ctx->iris_reset_gpio = devm_gpiod_get_optional(dev, "iris-reset",
						       GPIOD_OUT_LOW);
	if (IS_ERR(ctx->iris_reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->iris_reset_gpio),
				     "Failed to get iris-reset-gpios\n");

	ctx->iris_wakeup_gpio = devm_gpiod_get_optional(dev, "iris-wakeup",
							GPIOD_OUT_LOW);
	if (IS_ERR(ctx->iris_wakeup_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->iris_wakeup_gpio),
				     "Failed to get iris-wakeup-gpios\n");

	ctx->dsi = dsi;
	ctx->pwm_mode = -1;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	/*
	 * qcom,mdss-dsi-panel-clockrate of the 144 Hz vendor timing, the
	 * preferred mode. The panel wants each frame well within the frame
	 * time (mdp-transfer-time-us 5000 at 144 Hz, 9000 at 90 Hz), not
	 * spread over all of it: a link run at the refresh rate's pace left
	 * the panel at about 60 Hz at 90 Hz. The 998.4 MHz of the 60, 90 and
	 * 120 Hz timings is too slow at 144 Hz: a frame then takes 6.9 ms of
	 * the 6.94 ms frame time, and now and then the DPU never got the
	 * frame done again, freezing the display.
	 */
	dsi->hs_rate = 1148000000;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = tianma_nt37705_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	/* TODO: Pass slice_per_pkt = 2 */
	ctx->dsc.slice_height = 12;
	ctx->dsc.slice_width = 620;
	/*
	 * TODO: hdisplay should be read from the selected mode once
	 * it is passed back to drm_panel (in prepare?)
	 */
	WARN_ON(1240 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 1240 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;
	/*
	 * The panel decoder is configured for DSC 1.1 SCR (vendor register
	 * 0x91 in the on sequence: initial_dec_delay 645, scale_increment
	 * 271, nfl_bpg_offset 2793, slice_bpg_offset 1887, final_offset
	 * 4336), which needs first_line_bpg_offset = 15. The pre-SCR
	 * default (12) gives 567/307/2235 and a garbled, washed-out image.
	 */
	ctx->dsc.first_line_bpg_offset = 15;

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void tianma_nt37705_remove(struct mipi_dsi_device *dsi)
{
	struct tianma_nt37705 *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id tianma_nt37705_of_match[] = {
	{ .compatible = "tianma,nt37705" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tianma_nt37705_of_match);

static struct mipi_dsi_driver tianma_nt37705_driver = {
	.probe = tianma_nt37705_probe,
	.remove = tianma_nt37705_remove,
	.driver = {
		.name = "panel-tianma-nt37705",
		.of_match_table = tianma_nt37705_of_match,
	},
};
module_mipi_dsi_driver(tianma_nt37705_driver);

MODULE_AUTHOR("Danila Tikhonov <danila@jiaxyga.com>");
MODULE_DESCRIPTION("DRM driver for Tianma NT37705 cmd mode dsi panel");
MODULE_LICENSE("GPL");