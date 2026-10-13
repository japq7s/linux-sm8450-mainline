/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Based of tfa9895.c
 * Register definitions taken from tfa98xx kernel driver:
 * Copyright (C) 2014-2020 NXP Semiconductors, All Rights Reserved.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/soc.h>

#define TFA987X_SYS_CTRL0		0x00
#define TFA987X_SYS_CTRL0_PWDN_MSK	BIT(0)
#define TFA987X_SYS_CTRL0_I2CR_MSK	BIT(1)
#define TFA987X_SYS_CTRL0_AMPE_MSK	BIT(3)
#define TFA987X_SYS_CTRL0_DCDC_MSK	BIT(4)

#define TFA987X_SYS_CTRL1		0x01
#define TFA987X_SYS_CTRL1_MANSCONF_MSK	BIT(2)
#define TFA987X_SYS_CTRL1_MANSAOOSC_MSK	BIT(4)

#define TFA987X_SYS_CTRL2		0x02
#define TFA987X_SYS_CTRL2_AUDFS_MSK	GENMASK(3, 0)
#define TFA987X_SYS_CTRL2_FRACTDEL_MSK	GENMASK(10, 5)

#define TFA987X_REV			0x03
#define TFA987X_CLK_GATING_CTRL		0x05

#define TFA987X_TDM_CFG0		0x20
#define TFA987X_TDM_CFG0_FSBCLKS_MSK	GENMASK(15, 12)
#define TFA987X_TDM_CFG1		0x21
#define TFA987X_TDM_CFG1_NSLOTS_MSK	GENMASK(3,  0)
#define TFA987X_TDM_CFG1_SLOTBITS_MSK	GENMASK(8,  4)
#define TFA987X_TDM_CFG2		0x22
#define TFA987X_TDM_CFG2_SWIDTH_MSK	GENMASK(6,  2)
#define TFA987X_TDM_CFG3		0x23
#define TFA987X_TDM_CFG3_SPKE_MSK	BIT(0)
#define TFA987X_TDM_CFG3_DCE_MSK	BIT(1)
#define TFA987X_TDM_CFG3_CSE_MSK	BIT(3)
#define TFA987X_TDM_CFG3_VSE_MSK	BIT(4)
#define TFA987X_TDM_CFG6		0x26
#define TFA987X_TDM_CFG6_SPKS_MSK	GENMASK(3,  0)
#define TFA987X_TDM_CFG6_DCS_MSK	GENMASK(4,  7)
#define TFA987X_TDM_CFG6_CSS_MSK	GENMASK(15, 12)
#define TFA987X_TDM_CFG7		0x27
#define TFA987X_TDM_CFG7_VSS_MSK	GENMASK(3,  0)

#define TFA987X_AUDIO_CTRL		0x51
#define TFA987X_AUDIO_CTRL_BSSS_MSK	BIT(0)
#define TFA9872_AUDIO_CTRL_INTSMUTE_MSK	BIT(1)
#define TFA987X_AUDIO_CTRL_HPFBYP_MSK	BIT(5)
#define TFA987X_AUDIO_CTRL_DPSA_MSK     BIT(7)

#define TFA987X_AMP_CFG			0x52
#define TFA987X_AMP_CFG_CLIPCTRL_MSK	GENMASK(4, 2)
#define TFA987X_AMP_CFG_GAIN_MSK	GENMASK(12, 5)
#define TFA987X_AMP_CFG_SLOPEE_MSK	BIT(13)
#define TFA987X_AMP_CFG_SLOPESET_MSK	BIT(14)

#define TFA987X_KEY1_PWM_CFG		0x58
#define TFA987X_TDM_CFG8		0x61
#define TFA987X_TDM_CFG8_DCG_MSK	GENMASK(5, 2)
#define TFA987X_TDM_CFG8_SPKG_MSK	GENMASK(9, 6)

#define TFA987X_LOW_NOISE_GAIN1		0x62
#define TFA987X_LOW_NOISE_GAIN2		0x63
#define TFA987X_MODE1_DET1		0x64
#define TFA987X_MODE1_DET1_LPM1MODE_MSK	GENMASK(15, 14)
#define TFA987X_MODE1_DETECTOR2		0x65
#define TFA987X_TDM_SRC			0x68
#define TFA987X_CURSENSE_COMP		0x6f
#define TFA987X_DCDC_CTRL0		0x70
#define TFA9872_DCDC_CTRL0_DCVOS_MSK	GENMASK(2,  0)
#define TFA987X_DCDC_CTRL0_MCC_MSK	GENMASK(6,  3)
#define TFA987X_DCDC_CTRL0_DCIE_MSK	BIT(9)
#define TFA987X_DCDC_CTRL1		0x71
#define TFA987X_DCDC_CTRL4		0x74
#define TFA9872_DCDC_CTRL4_DCVOF_MSK	GENMASK(2,  0)
#define TFA987X_DCDC_CTRL4_DCTRIP_MSK	GENMASK(8,  4)
#define TFA987X_DCDC_CTRL5		0x75
#define TFA987X_DCDC_CTRL5_DCTRIP2_MSK	GENMASK(7,  3)
#define TFA987X_DCDC_CTRL6		0x76
#define TFA9874_DCDC_CTRL6_DCVOF_MSK	GENMASK(8,  3)
#define TFA9874_DCDC_CTRL6_DCVOS_MSK	GENMASK(14,  9)

/*
 * NXP bitfield id, as in the tfa98xx driver and in the tuning container:
 * register address in bits 15..8, LSB position in bits 7..4, width - 1 in
 * bits 3..0.
 */
#define TFA987X_BF_REG(bf)		((bf) >> 8)
#define TFA987X_BF_POS(bf)		(((bf) >> 4) & 0xf)
#define TFA987X_BF_LEN(bf)		(((bf) & 0xf) + 1)

enum {
	TFA987X_PROFILE_SPEAKER,
	TFA987X_PROFILE_RECEIVER,
	TFA987X_N_PROFILES,
};

struct tfa987x {
	struct regmap *rmap;
	/* <bitfield value> pairs from the devicetree, per profile */
	u32 *profile_bf[TFA987X_N_PROFILES];
	int profile_n[TFA987X_N_PROFILES];
	unsigned int profile;
};

static int tfa987x_write_bitfields(struct regmap *rmap, const u32 *bf, int n)
{
	int i, ret;

	for (i = 0; i < n; i += 2) {
		unsigned int pos = TFA987X_BF_POS(bf[i]);
		unsigned int mask = GENMASK(pos + TFA987X_BF_LEN(bf[i]) - 1, pos);

		ret = regmap_update_bits(rmap, TFA987X_BF_REG(bf[i]), mask,
					 bf[i + 1] << pos);
		if (ret)
			return ret;
	}
	return 0;
}

/* Reads "<name>" = <bitfield value>...; NULL with *n = 0 if absent */
static u32 *tfa987x_read_bitfields(struct device *dev, const char *name, int *n)
{
	u32 *bf;
	int i, ret;

	*n = 0;
	ret = of_property_count_u32_elems(dev->of_node, name);
	if (ret <= 0)
		return NULL;
	if (ret % 2)
		return ERR_PTR(-EINVAL);

	bf = devm_kcalloc(dev, ret, sizeof(*bf), GFP_KERNEL);
	if (!bf)
		return ERR_PTR(-ENOMEM);
	*n = ret;
	of_property_read_u32_array(dev->of_node, name, bf, *n);

	for (i = 0; i < *n; i += 2) {
		if (bf[i] > 0xffff ||
		    TFA987X_BF_POS(bf[i]) + TFA987X_BF_LEN(bf[i]) > 16 ||
		    bf[i + 1] >> TFA987X_BF_LEN(bf[i])) {
			dev_err(dev, "%s: bad bitfield 0x%x = 0x%x\n",
				name, bf[i], bf[i + 1]);
			return ERR_PTR(-EINVAL);
		}
	}
	return bf;
}

static int tfa987x_digital_mute(struct snd_soc_dai *codec_dai, int mute, int stream)
{
	struct snd_soc_component *component = codec_dai->component;
	int val = mute ? 0 : TFA987X_SYS_CTRL0_AMPE_MSK;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	snd_soc_component_update_bits(component, TFA987X_SYS_CTRL0,
						 TFA987X_SYS_CTRL0_AMPE_MSK, val);

	return 0;
}

/*
 * "<prefix> Amp Switch": lets userspace power down one amplifier of a pair
 * sharing the same I2S link, e.g. the loudspeaker during an earpiece call.
 * Virtual and inverted, so it defaults to on.
 */
static const struct snd_kcontrol_new tfa987x_amp_switch =
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 1);

static const char * const tfa987x_profile_texts[] = { "speaker", "receiver" };

static SOC_ENUM_SINGLE_VIRT_DECL(tfa987x_profile_enum, tfa987x_profile_texts);

static int tfa987x_profile_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa987x *tfa = snd_soc_component_get_drvdata(component);

	ucontrol->value.enumerated.item[0] = tfa->profile;
	return 0;
}

/*
 * Switch the amplifier settings between loudspeaker and earpiece use, as
 * the NXP driver does with the profiles of the tuning container: with the
 * amplifier powered down, then back to the previous power state.
 */
static int tfa987x_profile_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa987x *tfa = snd_soc_component_get_drvdata(component);
	unsigned int profile = ucontrol->value.enumerated.item[0];
	unsigned int ctrl0;
	int ret;

	if (profile >= TFA987X_N_PROFILES)
		return -EINVAL;
	if (profile == tfa->profile)
		return 0;

	ret = regmap_read(tfa->rmap, TFA987X_SYS_CTRL0, &ctrl0);
	if (ret)
		return ret;
	if (!(ctrl0 & TFA987X_SYS_CTRL0_PWDN_MSK))
		regmap_update_bits(tfa->rmap, TFA987X_SYS_CTRL0,
				   TFA987X_SYS_CTRL0_PWDN_MSK,
				   TFA987X_SYS_CTRL0_PWDN_MSK);

	ret = tfa987x_write_bitfields(tfa->rmap, tfa->profile_bf[profile],
				      tfa->profile_n[profile]);

	if (!(ctrl0 & TFA987X_SYS_CTRL0_PWDN_MSK))
		regmap_update_bits(tfa->rmap, TFA987X_SYS_CTRL0,
				   TFA987X_SYS_CTRL0_PWDN_MSK, 0);
	if (ret)
		return ret;

	tfa->profile = profile;
	return 1;
}

static const struct snd_kcontrol_new tfa987x_profile_control =
	SOC_ENUM_EXT("Profile", tfa987x_profile_enum,
		     tfa987x_profile_get, tfa987x_profile_put);

/* "<prefix> Profile" only exists when the devicetree describes a receiver */
static int tfa987x_component_probe(struct snd_soc_component *component)
{
	struct tfa987x *tfa = snd_soc_component_get_drvdata(component);

	if (!tfa->profile_n[TFA987X_PROFILE_RECEIVER])
		return 0;
	return snd_soc_add_component_controls(component,
					      &tfa987x_profile_control, 1);
}

static const struct snd_soc_dapm_widget tfa987x_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("Speaker"),
	SND_SOC_DAPM_SWITCH("Amp", SND_SOC_NOPM, 0, 0, &tfa987x_amp_switch),
	SND_SOC_DAPM_OUT_DRV_E("PWUP", TFA987X_SYS_CTRL0, 0, 1, NULL, 0, NULL, 0),
};

static const struct snd_soc_dapm_route tfa987x_dapm_routes[] = {
	{"Amp", "Switch", "HiFi Playback"},
	{"PWUP", NULL, "Amp"},
	{"Speaker", NULL, "PWUP"},
};

static const struct snd_soc_component_driver tfa987x_component = {
	.probe			= tfa987x_component_probe,
	.dapm_widgets		= tfa987x_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tfa987x_dapm_widgets),
	.dapm_routes		= tfa987x_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(tfa987x_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct snd_soc_dai_ops tfa987x_dai_ops = {
	.mute_stream = tfa987x_digital_mute,
};

static struct snd_soc_dai_driver tfa987x_dai = {
	.name = "tfa987x-hifi",
	.playback = {
		.stream_name	= "HiFi Playback",
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
		.rates		= SNDRV_PCM_RATE_48000,
		.rate_min	= 48000,
		.rate_max	= 48000,
		.channels_min	= 1,
		.channels_max	= 2,
	},
	.ops = &tfa987x_dai_ops,
};

static const struct regmap_config tfa987x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0xff,
};

static bool tfa987x_setup_dcdc(struct device *dev, struct regmap *rmap, u16 rev)
{
	u32 mcc, dcvof, dcvos, dctrip, dctrip2 = 0;
	int i;
	u32 *dest[] = { &mcc, &dcvof, &dcvos, &dctrip, &dctrip2 };
	const char *props[] = { "max-coil-current",
	      "first-boost-voltage", "second-boost-voltage",
	      "first-boost-trip-lvl", "second-boost-trip-lvl" };

	for (i = 0; i < ARRAY_SIZE(props); i++) {
		int ret;
		ret = of_property_read_u32(dev->of_node, props[i], dest[i]);
		if (ret < 0 && ret != -EINVAL)
			return false;
	}

	if (!FIELD_FIT(TFA987X_DCDC_CTRL0_MCC_MSK, mcc) ||
	    !FIELD_FIT(TFA987X_DCDC_CTRL4_DCTRIP_MSK, dctrip) ||
	    !FIELD_FIT(TFA987X_DCDC_CTRL5_DCTRIP2_MSK, dctrip2))
		return false;

	switch (rev & 0xff) {
	case 0x72:
		if (!FIELD_FIT(TFA9872_DCDC_CTRL4_DCVOF_MSK, dcvof) ||
		    !FIELD_FIT(TFA9872_DCDC_CTRL0_DCVOS_MSK, dcvos))
			return false;

		if (dcvof)
			regmap_update_bits(rmap, TFA987X_DCDC_CTRL4,
					TFA9872_DCDC_CTRL4_DCVOF_MSK,
					FIELD_PREP(TFA9872_DCDC_CTRL4_DCVOF_MSK, dcvof));
		if (dcvos)
			regmap_update_bits(rmap, TFA987X_DCDC_CTRL0,
					TFA9872_DCDC_CTRL0_DCVOS_MSK,
					FIELD_PREP(TFA9872_DCDC_CTRL0_DCVOS_MSK, dcvos));
		break;
	case 0x73:
	case 0x74:
		if (!FIELD_FIT(TFA9874_DCDC_CTRL6_DCVOF_MSK, dcvof) ||
		    !FIELD_FIT(TFA9874_DCDC_CTRL6_DCVOS_MSK, dcvos))
			return false;

		if (dcvof)
			regmap_update_bits(rmap, TFA987X_DCDC_CTRL6,
					TFA9874_DCDC_CTRL6_DCVOF_MSK,
					FIELD_PREP(TFA9874_DCDC_CTRL6_DCVOF_MSK, dcvof));

		if (dcvos)
			regmap_update_bits(rmap, TFA987X_DCDC_CTRL0,
					TFA9874_DCDC_CTRL6_DCVOS_MSK,
					FIELD_PREP(TFA9874_DCDC_CTRL6_DCVOS_MSK, dcvos));
		break;
	default:
		return false;
	}

	if (dctrip)
		regmap_update_bits(rmap, TFA987X_DCDC_CTRL4,
					 TFA987X_DCDC_CTRL4_DCTRIP_MSK,
					 FIELD_PREP(TFA987X_DCDC_CTRL4_DCTRIP_MSK, dctrip));
	if (dctrip2)
		regmap_update_bits(rmap, TFA987X_DCDC_CTRL5,
					 TFA987X_DCDC_CTRL5_DCTRIP2_MSK,
					 FIELD_PREP(TFA987X_DCDC_CTRL5_DCTRIP2_MSK, dctrip2));
	if (mcc)
		regmap_update_bits(rmap, TFA987X_DCDC_CTRL0,
					 TFA987X_DCDC_CTRL0_MCC_MSK |
					 TFA987X_DCDC_CTRL0_DCIE_MSK,
					 FIELD_PREP(TFA987X_DCDC_CTRL0_MCC_MSK, mcc) |
					 TFA987X_DCDC_CTRL0_DCIE_MSK);
	return true;
}

struct tfa987x_reg_seq {
	u8 reg;
	u16 val;
};

/*
 * Factory settings per revision, written after a reset. Generated code from
 * the NXP tfa98xx driver (tfa9874_specific(), tfa_init.c).
 */
static const struct tfa987x_reg_seq tfa9874_n1a_init[] = {	/* 0x0a74, V25 */
	{ 0x02, 0x22a8 }, { 0x51, 0x0020 }, { 0x52, 0x57dc }, { 0x58, 0x16a4 },
	{ 0x61, 0x0110 }, { 0x66, 0x0701 }, { 0x6f, 0x00a3 }, { 0x70, 0x07f8 },
	{ 0x73, 0x0007 }, { 0x74, 0x5068 }, { 0x75, 0x0d28 }, { 0x83, 0x0594 },
	{ 0x84, 0x0001 }, { 0x85, 0x0001 }, { 0x88, 0x0000 }, { 0xc4, 0x2001 },
};

static const struct tfa987x_reg_seq tfa9874_n1b_init[] = {	/* 0x0b74, V1.6 */
	{ 0x02, 0x22a8 }, { 0x51, 0x0020 }, { 0x52, 0x57dc }, { 0x58, 0x16a4 },
	{ 0x61, 0x0110 }, { 0x66, 0x0701 }, { 0x6f, 0x00a3 }, { 0x70, 0x07f8 },
	{ 0x73, 0x0047 }, { 0x74, 0x5068 }, { 0x75, 0x0d28 }, { 0x83, 0x0595 },
	{ 0x84, 0x0001 }, { 0x85, 0x0001 }, { 0x88, 0x0000 }, { 0xc4, 0x2001 },
};

static const struct tfa987x_reg_seq tfa9874_n1c_init[] = {	/* 0x0c74 */
	{ 0x02, 0x22c8 }, { 0x52, 0x57dc }, { 0x53, 0x003e }, { 0x56, 0x0400 },
	{ 0x61, 0x0110 }, { 0x6f, 0x00a5 }, { 0x70, 0x07f8 }, { 0x73, 0x0047 },
	{ 0x74, 0x5098 }, { 0x75, 0x8d28 }, { 0x80, 0x0000 }, { 0x83, 0x0799 },
	{ 0x84, 0x0081 },
};

static void tfa9874_init(struct regmap *rmap, unsigned int rev)
{
	const struct tfa987x_reg_seq *seq;
	unsigned int val;
	int i, n;

	switch (rev) {
	case 0x0a74:
		seq = tfa9874_n1a_init;
		n = ARRAY_SIZE(tfa9874_n1a_init);
		break;
	case 0x0b74:
		seq = tfa9874_n1b_init;
		n = ARRAY_SIZE(tfa9874_n1b_init);
		break;
	case 0x0c74:
		seq = tfa9874_n1c_init;
		n = ARRAY_SIZE(tfa9874_n1c_init);
		break;
	default:
		return;
	}

	/* Unlock key 1 and 2, as tfa9874_specific() + tfa98xx_key2(tfa, 0) */
	regmap_write(rmap, 0x0f, 0x5a6b);
	regmap_read(rmap, 0xfb, &val);
	regmap_write(rmap, 0xa0, val ^ 0x005a);
	regmap_write(rmap, 0x0f, 0x5a6b);
	regmap_update_bits(rmap, 0xa1, 0xff, 0x5a);
	regmap_write(rmap, 0x0f, 0);

	for (i = 0; i < n; i++)
		regmap_write(rmap, seq[i].reg, seq[i].val);
}

/* TFA9873 N1B factory settings, tfa9873_specific() "version 16" */
static const struct tfa987x_reg_seq tfa9873_n1b_init[] = {	/* 0x0b73 */
	{ 0x02, 0x0628 }, { 0x61, 0x0183 }, { 0x63, 0x005a }, { 0x6f, 0x0082 },
	{ 0x70, 0xa3eb }, { 0x73, 0x0187 }, { 0x83, 0x071c }, { 0x85, 0x0380 },
	{ 0xd5, 0x004d },
};

#define TFA9873_FSSYNCEN		0x0480

static void tfa9873_init(struct regmap *rmap)
{
	static const u32 no_fs_sync[] = { TFA9873_FSSYNCEN, 0 };
	unsigned int val;
	int i;

	/*
	 * Unlock key 1 and 2. The TFA9873 keeps the key registers visible
	 * ("advance keys handling" in tfa98xx), so 0x0f stays unlocked.
	 */
	regmap_write(rmap, 0x0f, 0x5a6b);
	regmap_read(rmap, 0xfb, &val);
	regmap_write(rmap, 0xa0, val ^ 0x005a);
	regmap_update_bits(rmap, 0xa1, 0xff, 0x5a);

	for (i = 0; i < ARRAY_SIZE(tfa9873_n1b_init); i++)
		regmap_write(rmap, tfa9873_n1b_init[i].reg,
			     tfa9873_n1b_init[i].val);

	/* tfa9873_specific() disables FS synchronisation */
	tfa987x_write_bitfields(rmap, no_fs_sync, ARRAY_SIZE(no_fs_sync));
}

static int tfa987x_i2c_probe(struct i2c_client *i2c)
{
	static const char * const profile_props[TFA987X_N_PROFILES] = {
		"nxp,speaker-bitfields", "nxp,receiver-bitfields" };
	struct device *dev = &i2c->dev;
	struct gpio_desc *reset;
	struct regmap *rmap;
	struct tfa987x *tfa;
	unsigned int rev;
	u32 *init_bf;
	int init_n, i;
	u32 slot = 0;
	int ret;

	tfa = devm_kzalloc(dev, sizeof(*tfa), GFP_KERNEL);
	if (!tfa)
		return -ENOMEM;
	dev_set_drvdata(dev, tfa);

	/* Board tuning, the bitfields of the vendor's tuning container */
	init_bf = tfa987x_read_bitfields(dev, "nxp,init-bitfields", &init_n);
	if (IS_ERR(init_bf))
		return PTR_ERR(init_bf);
	for (i = 0; i < TFA987X_N_PROFILES; i++) {
		tfa->profile_bf[i] = tfa987x_read_bitfields(dev, profile_props[i],
							    &tfa->profile_n[i]);
		if (IS_ERR(tfa->profile_bf[i]))
			return PTR_ERR(tfa->profile_bf[i]);
	}
	if (tfa->profile_n[TFA987X_PROFILE_RECEIVER] &&
	    !tfa->profile_n[TFA987X_PROFILE_SPEAKER])
		return dev_err_probe(dev, -EINVAL,
				     "receiver bitfields without speaker bitfields\n");

	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "Failed to enable vdd\n");

	/* Optional external reset (active high): pulse it, as NXP does */
	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "Failed to get reset\n");
	if (reset) {
		usleep_range(5000, 6000);
		gpiod_set_value_cansleep(reset, 0);
		usleep_range(5000, 6000);
	}

	/* TDM/I2S slot this amplifier plays (0 = left, 1 = right) */
	of_property_read_u32(dev->of_node, "nxp,tdm-slot", &slot);

	rmap = devm_regmap_init_i2c(i2c, &tfa987x_regmap_config);
	if (IS_ERR(rmap))
		return PTR_ERR(rmap);
	tfa->rmap = rmap;

	ret = regmap_read(rmap, TFA987X_REV, &rev);
	if (ret < 0) {
		dev_err(dev, "Failed to read revision register: %d\n", ret);
		return ret;
	}

	switch (rev) {
		case 0x0b73:
		case 0x1b72:
		case 0x2b72:
		case 0x3b72:
		case 0x0a74:
		case 0x0b74:
		case 0x0c74:
			dev_info(dev, "Chip revision: 0x%04x\n", rev);
			break;
		default:
			dev_err(dev, "Unsupported chip revision: 0x%04x\n", rev);
			return -ENODEV;
	}

	/* Perform soft reset */
	regmap_write(rmap, TFA987X_SYS_CTRL0, TFA987X_SYS_CTRL0_I2CR_MSK);

	if ((rev & 0xff) == 0x74)
		tfa9874_init(rmap, rev);
	else if (rev == 0x0b73)
		tfa9873_init(rmap);

	/* Setup DC-DC Converter if we have configuration */
	regmap_update_bits(rmap, TFA987X_SYS_CTRL0, TFA987X_SYS_CTRL0_DCDC_MSK,
			   FIELD_PREP(TFA987X_SYS_CTRL0_DCDC_MSK,
				   (u16) tfa987x_setup_dcdc(dev, rmap, rev)));

	/* Disable DPSA */
	regmap_update_bits(rmap, TFA987X_AUDIO_CTRL,
				 TFA987X_AUDIO_CTRL_DPSA_MSK, 0);

	/* Setup TDM 16 bit 1 slot config */
	regmap_update_bits(rmap, TFA987X_TDM_CFG0,
				 TFA987X_TDM_CFG0_FSBCLKS_MSK,
				 FIELD_PREP(TFA987X_TDM_CFG0_FSBCLKS_MSK, 0));
	regmap_update_bits(rmap, TFA987X_TDM_CFG1,
				 TFA987X_TDM_CFG1_NSLOTS_MSK |
				 TFA987X_TDM_CFG1_SLOTBITS_MSK,
				 FIELD_PREP(TFA987X_TDM_CFG1_NSLOTS_MSK, 1) |
				 FIELD_PREP(TFA987X_TDM_CFG1_SLOTBITS_MSK, 15));
	regmap_update_bits(rmap, TFA987X_TDM_CFG2,
				 TFA987X_TDM_CFG2_SWIDTH_MSK,
				 FIELD_PREP(TFA987X_TDM_CFG2_SWIDTH_MSK, 15));

	/* No current/voltage sense over TDM */
	regmap_update_bits(rmap, TFA987X_TDM_CFG3,
				 TFA987X_TDM_CFG3_SPKE_MSK |
				 TFA987X_TDM_CFG3_DCE_MSK |
				 TFA987X_TDM_CFG3_CSE_MSK |
				 TFA987X_TDM_CFG3_VSE_MSK,
				 TFA987X_TDM_CFG3_SPKE_MSK);
	regmap_update_bits(rmap, TFA987X_TDM_CFG6,
				 TFA987X_TDM_CFG6_SPKS_MSK,
				 FIELD_PREP(TFA987X_TDM_CFG6_SPKS_MSK, slot));

	if ((rev & 0xff) == 0x72)
		regmap_update_bits(rmap, TFA987X_MODE1_DET1,
				 TFA987X_MODE1_DET1_LPM1MODE_MSK,
				 FIELD_PREP(TFA987X_MODE1_DET1_LPM1MODE_MSK, 1));

	ret = tfa987x_write_bitfields(rmap, init_bf, init_n);
	if (!ret)
		ret = tfa987x_write_bitfields(rmap,
				tfa->profile_bf[TFA987X_PROFILE_SPEAKER],
				tfa->profile_n[TFA987X_PROFILE_SPEAKER]);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write the tuning\n");

	/* Turn on this thing */
	regmap_update_bits(rmap, TFA987X_SYS_CTRL0,
				 TFA987X_SYS_CTRL0_PWDN_MSK, 0);

	regmap_update_bits(rmap, TFA987X_SYS_CTRL1,
				 TFA987X_SYS_CTRL1_MANSCONF_MSK,
				 TFA987X_SYS_CTRL1_MANSCONF_MSK);

	return devm_snd_soc_register_component(dev, &tfa987x_component,
						    &tfa987x_dai, 1);
}

static const struct of_device_id tfa987x_of_match[] = {
	{ .compatible = "nxp,tfa9872" },
	{ .compatible = "nxp,tfa9874" },
	{ }
};
MODULE_DEVICE_TABLE(of, tfa987x_of_match);

static struct i2c_driver tfa987x_i2c_driver = {
	.driver = {
		.name = "tfa987x",
		.of_match_table = tfa987x_of_match,
	},
	.probe = tfa987x_i2c_probe,
};
module_i2c_driver(tfa987x_i2c_driver);

MODULE_DESCRIPTION("ASoC NXP Semiconductors TFA9872/TFA9874 driver");
MODULE_LICENSE("GPL v2");
