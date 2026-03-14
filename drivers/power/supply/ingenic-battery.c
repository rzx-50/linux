// SPDX-License-Identifier: GPL-2.0
/*
 * Battery driver for the Ingenic JZ47xx SoCs
 * Copyright (c) 2019 Artur Rojek <contact@artur-rojek.eu>
 *
 * Based on drivers/power/supply/jz4740-battery.c
 */

#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>

struct ingenic_battery {
    struct device *dev;
    struct iio_channel *channel;
    struct power_supply_desc desc;
    struct power_supply *battery;
    struct power_supply_battery_info *info;
};

static int ingenic_battery_get_property(struct power_supply *psy,
                                        enum power_supply_property psp,
                                        union power_supply_propval *val)
{
    struct ingenic_battery *bat = power_supply_get_drvdata(psy);
    struct power_supply_battery_info *info = bat->info;
    int ret;

    switch (psp) {
    case POWER_SUPPLY_PROP_HEALTH:
        ret = iio_read_channel_processed(bat->channel, &val->intval);
        if (ret)
            return ret;

        val->intval *= 2640;

        if (val->intval < info->voltage_min_design_uv)
            val->intval = POWER_SUPPLY_HEALTH_DEAD;
        else if (val->intval > info->voltage_max_design_uv)
            val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
        else
            val->intval = POWER_SUPPLY_HEALTH_GOOD;

        return 0;
    case POWER_SUPPLY_PROP_VOLTAGE_NOW:
        ret = iio_read_channel_processed(bat->channel, &val->intval);
        if (ret)
            return ret;

        val->intval *= 2640;
        return 0;
    case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
        val->intval = info->voltage_min_design_uv;
        return 0;
    case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
        val->intval = info->voltage_max_design_uv;
        return 0;
    default:
        return -EINVAL;
    }
}

/*
 * Checks whether the given scale type is supported by this driver.
 */
static inline bool scale_type_supported(int type)
{
    switch (type) {
    case IIO_VAL_INT_PLUS_NANO:
    case IIO_VAL_INT_PLUS_MICRO:
    case IIO_VAL_FRACTIONAL_LOG2:
        return true;
    default:
        return false;
    }
}

/*
 * Selects and sets an appropriate voltage scaling factor in the IIO channel.
 */
static int ingenic_battery_set_scale(struct ingenic_battery *bat)
{
    const int *scale_raw;
    int scale_len, scale_type, best_idx = -1, best_mV, max_raw, i, ret;
    unsigned int offset;
    u64 max_mV, scale_mV;

    ret = iio_read_max_channel_raw(bat->channel, &max_raw);
    if (ret) {
        dev_err(bat->dev, "Failed to read maximum raw channel value\n");
        return ret;
    }

    ret = iio_read_avail_channel_attribute(bat->channel, &scale_raw,
                                           &scale_type, &scale_len,
                                           IIO_CHAN_INFO_SCALE);
    if (ret < 0) {
        dev_err(bat->dev, "Failed to read available scales for the channel\n");
        return ret;
    }

    switch (ret) {
    case IIO_AVAIL_LIST:
        if (!scale_type_supported(scale_type)) {
            dev_err(bat->dev, "Scale type is unsupported\n");
            return -EINVAL;
        }

        offset = 2;
        break;
    case IIO_AVAIL_LIST_WITH_TYPE:
        for (i = 0; i < scale_len; i += 3) {
            if (!scale_type_supported(scale_raw[i + 2])) {
                dev_err(bat->dev, "Scale type is unsupported\n");
                return -EINVAL;
            }
        }

        offset = 3;
        scale_type = scale_raw[2];
        break;
    default:
        dev_err(bat->dev, "Unsupported format of available scales\n");
        return -EINVAL;
    }

    max_mV = bat->info->voltage_max_design_uv / 1000;

    for (i = 0; i < scale_len; i += offset) {
        switch (scale_type) {
        case IIO_VAL_INT_PLUS_MICRO:
            scale_mV = max_raw * scale_raw[i]
                      + max_raw * scale_raw[i + 1] / 1000;
            break;
        case IIO_VAL_INT_PLUS_NANO:
            scale_mV = max_raw * scale_raw[i]
                      + max_raw * scale_raw[i + 1] / 1000000;
            break;
        case IIO_VAL_FRACTIONAL_LOG2:
            scale_mV = (max_raw * scale_raw[i]) >> scale_raw[i + 1];
            break;
        }

        if (scale_mV < max_mV)
            continue;

        if (best_idx >= 0 && scale_mV > best_mV)
            continue;

        best_mV = scale_mV;
        best_idx = i;
    }

    if (best_idx < 0) {
        dev_err(bat->dev, "Could not find suitable voltage scale\n");
        return -EINVAL;
    }

    if (scale_len > offset) {
        ret = iio_write_channel_attribute(bat->channel,
                                          scale_raw[best_idx],
                                          scale_raw[best_idx + 1],
                                          IIO_CHAN_INFO_SCALE);
        if (ret)
            return ret;
    }

    return 0;
}

static enum power_supply_property ingenic_battery_properties[] = {
    POWER_SUPPLY_PROP_HEALTH,
    POWER_SUPPLY_PROP_VOLTAGE_NOW,
    POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
    POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
};

static int ingenic_battery_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct ingenic_battery *bat;
    struct power_supply_config psy_cfg = {};
    struct power_supply_desc *desc;
    int ret;

    bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
    if (!bat)
        return -ENOMEM;

    bat->dev = dev;
    bat->channel = devm_iio_channel_get(dev, "battery");
    if (IS_ERR(bat->channel)) {
        dev_err(dev, "Failed to obtain IIO channel\n");
        return PTR_ERR(bat->channel);
    }

    desc = &bat->desc;
    desc->name = "jz-battery";
    desc->type = POWER_SUPPLY_TYPE_BATTERY;
    desc->properties = ingenic_battery_properties;
    desc->num_properties = ARRAY_SIZE(ingenic_battery_properties);
    desc->get_property = ingenic_battery_get_property;
    psy_cfg.drv_data = bat;
    psy_cfg.of_node = dev->of_node;

    bat->battery = devm_power_supply_register(dev, desc, &psy_cfg);
    if (IS_ERR(bat->battery)) {
        dev_err(dev, "Failed to register battery\n");
        return PTR_ERR(bat->battery);
    }

    ret = power_supply_get_battery_info(bat->battery, &bat->info);
    if (ret) {
        dev_err(dev, "Failed to retrieve battery information: %d\n", ret);
        return ret;
    }

    if (bat->info->voltage_min_design_uv < 0 ||
        bat->info->voltage_max_design_uv < 0) {
        dev_err(dev, "Invalid battery voltage values\n");
        return -EINVAL;
    }

    return ingenic_battery_set_scale(bat);
}

#ifdef CONFIG_OF
static const struct of_device_id ingenic_battery_of_match[] = {
    { .compatible = "ingenic,jz4740-battery", },
    {}
};
MODULE_DEVICE_TABLE(of, ingenic_battery_of_match);
#endif

static struct platform_driver ingenic_battery_driver = {
    .driver = {
        .name = "ingenic-battery",
        .of_match_table = of_match_ptr(ingenic_battery_of_match),
    },
    .probe = ingenic_battery_probe,
};
module_platform_driver(ingenic_battery_driver);

MODULE_DESCRIPTION("Battery driver for Ingenic JZ47xx SoCs");
MODULE_AUTHOR("Artur Rojek <contact@artur-rojek.eu>");
MODULE_LICENSE("GPL");