// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW21009/AW21012/AW21018 LED driver
 *
 * Only the constant current mode used by the realme GT Neo5 back panel light
 * is supported: the chip runs in group mode with a fixed global brightness,
 * and each colour is set through the scaling register of its channel. The
 * register sequence follows the Awinic/OPLUS downstream aw210xx driver.
 *
 * Copyright (C) 2026 japq7s <japiotr123@proton.me>
 */

#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define AW210XX_REG_GCR		0x20
#define AW210XX_REG_UPDATE	0x45
#define AW210XX_REG_SL(ch)	(0x46 + (ch))
#define AW210XX_REG_GCCR	0x58
#define AW210XX_REG_UVCR	0x60
#define AW210XX_REG_GCR2	0x61
#define AW210XX_REG_RESET	0x70
#define AW210XX_REG_ABMCFG	0x80
#define AW210XX_REG_ABMGO	0x81
#define AW210XX_REG_GBRH	0x86
#define AW210XX_REG_GBRL	0x87
#define AW210XX_REG_GCFG	0x8b

#define AW210XX_GCR_APSE	BIT(7)
#define AW210XX_GCR_CLKFRQ	GENMASK(6, 4)	/* 0: 16 MHz */
#define AW210XX_GCR_PWMRES	GENMASK(2, 1)	/* 0: 8 bit */
#define AW210XX_GCR_CHIPEN	BIT(0)
#define AW210XX_GCR2_SBMD	BIT(1)
#define AW210XX_GCR2_RGBMD	BIT(0)
#define AW210XX_UVCR_UVPD	BIT(1)
#define AW210XX_UVCR_UVDIS	BIT(0)

/* group mode as programmed by the downstream driver */
#define AW210XX_GCFG_GROUP	0x4f

#define AW21018_CHIPID		0x02
#define AW21012_CHIPID		0x22
#define AW21009_CHIPID		0x12

#define AW210XX_MAX_CHANNELS	18
#define AW210XX_GCCR_DEFAULT	0xcc
#define AW210XX_OFF_DELAY_MS	1000

struct aw210xx {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *vbled_gpio;
	struct led_classdev_mc mc;
	struct mc_subled *subleds;
	struct delayed_work off_work;
	struct mutex lock;
	bool powered;
	u32 global_current;
};

static const struct regmap_config aw210xx_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW210XX_REG_GCFG,
};

static int aw210xx_chip_init(struct aw210xx *chip)
{
	struct regmap *map = chip->regmap;
	int ret;

	ret = regmap_update_bits(map, AW210XX_REG_GCR, AW210XX_GCR_CHIPEN,
				 AW210XX_GCR_CHIPEN);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	ret = regmap_update_bits(map, AW210XX_REG_GCR2,
				 AW210XX_GCR2_SBMD | AW210XX_GCR2_RGBMD,
				 AW210XX_GCR2_SBMD);
	if (ret)
		return ret;

	ret = regmap_update_bits(map, AW210XX_REG_GCR,
				 AW210XX_GCR_APSE | AW210XX_GCR_CLKFRQ |
				 AW210XX_GCR_PWMRES, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(map, AW210XX_REG_UVCR,
				 AW210XX_UVCR_UVPD | AW210XX_UVCR_UVDIS, 0);
	if (ret)
		return ret;

	ret = regmap_write(map, AW210XX_REG_GCFG, AW210XX_GCFG_GROUP);
	if (ret)
		return ret;

	/* constant current: no pattern, all channels at full group brightness */
	ret = regmap_write(map, AW210XX_REG_ABMCFG, 0);
	if (!ret)
		ret = regmap_write(map, AW210XX_REG_ABMGO, 0);
	if (!ret)
		ret = regmap_write(map, AW210XX_REG_GBRH, 0);
	if (!ret)
		ret = regmap_write(map, AW210XX_REG_GBRL, 0xff);
	if (!ret)
		ret = regmap_write(map, AW210XX_REG_GCCR, chip->global_current);

	return ret;
}

static void aw210xx_power_off(struct aw210xx *chip)
{
	if (!chip->powered)
		return;

	gpiod_set_value_cansleep(chip->vbled_gpio, 0);
	gpiod_set_value_cansleep(chip->enable_gpio, 0);
	chip->powered = false;
}

static int aw210xx_power_on(struct aw210xx *chip)
{
	int ret;

	if (chip->powered)
		return 0;

	gpiod_set_value_cansleep(chip->enable_gpio, 1);
	gpiod_set_value_cansleep(chip->vbled_gpio, 1);
	usleep_range(2000, 2500);
	chip->powered = true;

	ret = aw210xx_chip_init(chip);
	if (ret) {
		dev_err(chip->dev, "failed to initialise: %d\n", ret);
		aw210xx_power_off(chip);
	}

	return ret;
}

static void aw210xx_off_work(struct work_struct *work)
{
	struct aw210xx *chip = container_of(work, struct aw210xx,
					    off_work.work);

	guard(mutex)(&chip->lock);
	if (!chip->mc.led_cdev.brightness)
		aw210xx_power_off(chip);
}

static int aw210xx_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct aw210xx *chip = container_of(mc, struct aw210xx, mc);
	unsigned int i;
	int ret;

	guard(mutex)(&chip->lock);

	/*
	 * Blinking triggers switch the LED off between the on phases, so
	 * only cut the power once it has stayed off for a while.
	 */
	if (!brightness) {
		if (!chip->powered)
			return 0;
		mod_delayed_work(system_wq, &chip->off_work,
				 msecs_to_jiffies(AW210XX_OFF_DELAY_MS));
	} else {
		cancel_delayed_work(&chip->off_work);
		ret = aw210xx_power_on(chip);
		if (ret)
			return ret;
	}

	led_mc_calc_color_components(mc, brightness);

	for (i = 0; i < mc->num_colors; i++) {
		ret = regmap_write(chip->regmap,
				   AW210XX_REG_SL(mc->subled_info[i].channel),
				   mc->subled_info[i].brightness);
		if (ret)
			return ret;
	}

	return regmap_write(chip->regmap, AW210XX_REG_UPDATE, 0);
}

static int aw210xx_read_chipid(struct aw210xx *chip)
{
	unsigned int id;
	const char *name;
	int ret;

	ret = regmap_read(chip->regmap, AW210XX_REG_RESET, &id);
	if (ret)
		return dev_err_probe(chip->dev, ret, "failed to read chip id\n");

	switch (id) {
	case AW21018_CHIPID:
		name = "AW21018";
		break;
	case AW21012_CHIPID:
		name = "AW21012";
		break;
	case AW21009_CHIPID:
		name = "AW21009";
		break;
	default:
		return dev_err_probe(chip->dev, -ENODEV,
				     "unknown chip id 0x%02x\n", id);
	}

	dev_info(chip->dev, "%s (chip id 0x%02x)\n", name, id);
	return 0;
}

static int aw210xx_parse_led(struct aw210xx *chip,
			     struct fwnode_handle **led_node)
{
	struct device *dev = chip->dev;
	struct fwnode_handle *node;
	unsigned int num = 0;
	u32 color, channel;
	int ret;

	node = device_get_named_child_node(dev, "multi-led");
	if (!node)
		return dev_err_probe(dev, -EINVAL, "missing multi-led node\n");

	num = fwnode_get_child_node_count(node);
	if (!num || num > AW210XX_MAX_CHANNELS) {
		fwnode_handle_put(node);
		return dev_err_probe(dev, -EINVAL, "bad number of LEDs: %u\n",
				     num);
	}

	chip->subleds = devm_kcalloc(dev, num, sizeof(*chip->subleds),
				     GFP_KERNEL);
	if (!chip->subleds) {
		fwnode_handle_put(node);
		return -ENOMEM;
	}

	num = 0;
	fwnode_for_each_child_node_scoped(node, child) {
		ret = fwnode_property_read_u32(child, "reg", &channel);
		if (ret || channel >= AW210XX_MAX_CHANNELS) {
			fwnode_handle_put(node);
			return dev_err_probe(dev, -EINVAL,
					     "bad or missing reg\n");
		}

		ret = fwnode_property_read_u32(child, "color", &color);
		if (ret) {
			fwnode_handle_put(node);
			return dev_err_probe(dev, ret, "missing color\n");
		}

		chip->subleds[num].channel = channel;
		chip->subleds[num].color_index = color;
		chip->subleds[num].intensity = LED_FULL;
		num++;
	}

	chip->mc.subled_info = chip->subleds;
	chip->mc.num_colors = num;
	*led_node = node;

	return 0;
}

static void aw210xx_shutdown_action(void *data)
{
	struct aw210xx *chip = data;

	cancel_delayed_work_sync(&chip->off_work);
	guard(mutex)(&chip->lock);
	aw210xx_power_off(chip);
}

static int aw210xx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct led_init_data init_data = {};
	struct fwnode_handle *led_node;
	struct aw210xx *chip;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	chip->global_current = AW210XX_GCCR_DEFAULT;
	device_property_read_u32(dev, "awinic,global-current",
				 &chip->global_current);
	if (chip->global_current > 0xff)
		return dev_err_probe(dev, -EINVAL, "bad global current\n");

	ret = devm_mutex_init(dev, &chip->lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&chip->off_work, aw210xx_off_work);

	chip->regmap = devm_regmap_init_i2c(client, &aw210xx_regmap_config);
	if (IS_ERR(chip->regmap))
		return PTR_ERR(chip->regmap);

	chip->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(chip->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(chip->enable_gpio),
				     "failed to get enable gpio\n");

	chip->vbled_gpio = devm_gpiod_get_optional(dev, "vbled", GPIOD_OUT_LOW);
	if (IS_ERR(chip->vbled_gpio))
		return dev_err_probe(dev, PTR_ERR(chip->vbled_gpio),
				     "failed to get vbled gpio\n");

	ret = devm_add_action_or_reset(dev, aw210xx_shutdown_action, chip);
	if (ret)
		return ret;

	ret = aw210xx_power_on(chip);
	if (ret)
		return ret;

	ret = aw210xx_read_chipid(chip);
	aw210xx_power_off(chip);
	if (ret)
		return ret;

	ret = aw210xx_parse_led(chip, &led_node);
	if (ret)
		return ret;

	chip->mc.led_cdev.max_brightness = LED_FULL;
	chip->mc.led_cdev.brightness_set_blocking = aw210xx_brightness_set;
	init_data.fwnode = led_node;

	ret = devm_led_classdev_multicolor_register_ext(dev, &chip->mc,
							&init_data);
	fwnode_handle_put(led_node);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register LED\n");

	return 0;
}

static const struct of_device_id aw210xx_of_match[] = {
	{ .compatible = "awinic,aw210xx" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw210xx_of_match);

static const struct i2c_device_id aw210xx_id[] = {
	{ "aw210xx" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw210xx_id);

static struct i2c_driver aw210xx_driver = {
	.driver = {
		.name = "leds-aw210xx",
		.of_match_table = aw210xx_of_match,
	},
	.probe = aw210xx_probe,
	.id_table = aw210xx_id,
};
module_i2c_driver(aw210xx_driver);

MODULE_DESCRIPTION("Awinic AW210xx LED driver");
MODULE_AUTHOR("japq7s <japiotr123@proton.me>");
MODULE_LICENSE("GPL");
