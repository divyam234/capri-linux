/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Native Linux PMI632 SMB5 battery protection for Motorola Capri.
 *
 * This is a board-specific integration layer, not an Android 4.19 port.
 * QGauge and the thermal ADC provider use normal Linux power_supply/IIO.
 */
#ifndef QCOM_PMI632_POLICY_H
#define QCOM_PMI632_POLICY_H

#define PMI632_JEITA_HOT_SOFT	0x94
#define PMI632_JEITA_COLD_SOFT	0x96
#define PMI632_JEITA_HOT_HARD	0x98
#define PMI632_JEITA_COLD_HARD	0x9a
#define PMI632_JEITA_COMP_HOT	0x92
#define PMI632_JEITA_COMP_COLD	0x93

static int pmi632_charge_enable(struct smb_chip *chip, bool enabled)
{
	return regmap_update_bits(chip->regmap,
				chip->base + CHARGING_ENABLE_CMD,
				CHARGING_ENABLE_CMD_BIT,
				enabled ? CHARGING_ENABLE_CMD_BIT : 0);
}

static int pmi632_read_battery(int *temp, int *vbat, int *current_ua,
				int *present, int *health)
{
	struct power_supply *battery;
	union power_supply_propval val;
	int ret;

	battery = power_supply_get_by_name("battery");
	if (!battery)
		return -ENODEV;

	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_PRESENT, &val);
	if (ret)
		goto out;
	*present = val.intval;

	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_TEMP, &val);
	if (ret)
		goto out;
	*temp = val.intval;

	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_VOLTAGE_NOW,
					&val);
	if (ret)
		goto out;
	*vbat = val.intval;

	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	if (ret)
		goto out;
	*current_ua = val.intval;

	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_HEALTH, &val);
	if (!ret)
		*health = val.intval;
out:
	power_supply_put(battery);
	return ret;
}

static int pmi632_write_threshold(struct smb_chip *chip, u16 reg, int mv)
{
	u16 raw = DIV_ROUND_CLOSEST(mv * 0x70e4, 1875);
	u8 bytes[2] = { raw >> 8, raw & 0xff };

	return regmap_bulk_write(chip->regmap, chip->base + reg,
				 bytes, sizeof(bytes));
}

/*
 * Hardware JEITA is independent of software polling and must remain
 * enabled even when the kernel suspends. The values are for the
 * Motorola 10k NTC (B3435), PMI632 30k pullup.
 */
static int pmi632_init_protection(struct smb_chip *chip)
{
	const struct {
		u16 reg;
		int millivolts;
	} limits[] = {
		{ PMI632_JEITA_HOT_SOFT, 305 },	/* 40 C */
		{ PMI632_JEITA_COLD_SOFT, 702 },	/* 10 C */
		{ PMI632_JEITA_HOT_HARD, 264 },	/* 45 C */
		{ PMI632_JEITA_COLD_HARD, 795 },	/* 5 C */
	};
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(limits); i++) {
		ret = pmi632_write_threshold(chip, limits[i].reg,
					     limits[i].millivolts);
		if (ret)
			return ret;
	}

	/* A conservative 250 mA soft-temperature compensation. */
	ret = regmap_write(chip->regmap, chip->base + PMI632_JEITA_COMP_HOT, 10);
	if (ret)
		return ret;
	ret = regmap_write(chip->regmap, chip->base + PMI632_JEITA_COMP_COLD, 10);
	if (ret)
		return ret;

	/* SMB5 vendor driver uses bits 0..3; bit 4 belongs to SMB2. */
	ret = regmap_update_bits(chip->regmap, chip->base + JEITA_EN_CFG,
				 GENMASK(3, 0), GENMASK(3, 0));
	if (ret)
		return ret;

	/* Watchdog bite must disable charging if the safety worker stops. */
	ret = regmap_write(chip->regmap, chip->base + SNARL_BARK_BITE_WD_CFG,
				BITE_WDOG_DISABLE_CHARGING_CFG_BIT |
				BIT(2) | GENMASK(1, 0));
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, chip->base + WD_CFG,
				 WDOG_TIMER_EN_ON_PLUGIN_BIT | WDOG_TIMER_EN_BIT,
				 WDOG_TIMER_EN_ON_PLUGIN_BIT);
	if (ret)
		return ret;

	return regmap_write(chip->regmap, chip->base + BARK_BITE_WDOG_PET,
				    BARK_BITE_WDOG_PET_BIT);
}

static int pmi632_check_battery(struct smb_chip *chip, int *charge_ua)
{
	int temp = 0, voltage = 0, ibat_ua = 0, present = 0;
	int health = POWER_SUPPLY_HEALTH_UNKNOWN;
	unsigned int jeita, watchdog, bite;
	int ret;
	u8 threshold[2];
	u16 raw;

	ret = pmi632_read_battery(&temp, &voltage, &ibat_ua, &present, &health);
	if (ret)
		return ret;

	if (!present || voltage < 3200000 || temp < -400 || temp > 900)
		return -ERANGE;

	/* Use the SMB5 hardware charger health (including BAT_OV) via QGauge.
	 * A battery at its float target is not an overvoltage fault.
	 */
	if (health != POWER_SUPPLY_HEALTH_GOOD)
		return -ERANGE;

	/*
	 * A hard temperature fault latches charging off until the pack
	 * returns to 10-40 C; the independent hardware JEITA gate is 5-45 C.
	 */
	if (temp <= 50 || temp >= 450)
		chip->pmi632_temp_latched = true;
	else if (temp >= 100 && temp <= 400)
		chip->pmi632_temp_latched = false;
	if (chip->pmi632_temp_latched)
		return -ERANGE;

	/* Terminate after three low-current samples at the pack float voltage.
	 * Resume only after three samples 150mV below float. This is backed up
	 * by the hardware's own termination comparator and charge-inhibit logic.
	 */
	if (chip->pmi632_full_latched) {
		if (voltage <= chip->pmi632_float_uv - 150000)
			chip->pmi632_recharge_count++;
		else
			chip->pmi632_recharge_count = 0;
		if (chip->pmi632_recharge_count < 3)
			return -EALREADY;
		chip->pmi632_full_latched = false;
		chip->pmi632_recharge_count = 0;
	}
	if (voltage >= chip->pmi632_float_uv - 25000 &&
	    ibat_ua >= 0 && ibat_ua <= chip->pmi632_term_ua) {
		if (++chip->pmi632_full_count >= 3) {
			chip->pmi632_full_latched = true;
			return -EALREADY;
		}
	} else {
		chip->pmi632_full_count = 0;
	}

	*charge_ua = min(chip->pmi632_fcc_max_ua,
				  (temp < 150 || temp > 400) ? 500000 : 1500000);

	ret = regmap_read(chip->regmap, chip->base + JEITA_EN_CFG, &jeita);
	if (ret)
		return ret;
	if ((jeita & GENMASK(3, 0)) != GENMASK(3, 0))
		return -EIO;

	ret = regmap_bulk_read(chip->regmap,
				chip->base + PMI632_JEITA_HOT_HARD, threshold,
				sizeof(threshold));
	if (ret)
		return ret;
	raw = (threshold[0] << 8) | threshold[1];
	if (raw != DIV_ROUND_CLOSEST(264 * 0x70e4, 1875))
		return -EIO;
	ret = regmap_bulk_read(chip->regmap,
				chip->base + PMI632_JEITA_COLD_HARD, threshold,
				sizeof(threshold));
	if (ret)
		return ret;
	raw = (threshold[0] << 8) | threshold[1];
	if (raw != DIV_ROUND_CLOSEST(795 * 0x70e4, 1875))
		return -EIO;

	ret = regmap_read(chip->regmap, chip->base + WD_CFG, &watchdog);
	if (ret)
		return ret;
	ret = regmap_read(chip->regmap, chip->base + SNARL_BARK_BITE_WD_CFG, &bite);
	if (ret)
		return ret;
	if (!(watchdog & WDOG_TIMER_EN_ON_PLUGIN_BIT) ||
	    !(bite & BITE_WDOG_DISABLE_CHARGING_CFG_BIT))
		return -EIO;

	return 0;
}

static void pmi632_safety_work(struct work_struct *work)
{
	struct smb_chip *chip = container_of(to_delayed_work(work),
					      struct smb_chip, pmi632_safety_work);
	int current_ua = 0;
	bool was_full;
	int ret;

	mutex_lock(&chip->pmi632_lock);
	if (chip->pmi632_suspended)
		goto out;

	was_full = chip->pmi632_full_latched;
	ret = pmi632_check_battery(chip, &current_ua);
	if (ret) {
		if (pmi632_charge_enable(chip, false))
			dev_err_ratelimited(chip->dev, "failed to inhibit PMI632 charging\n");
	} else {
		ret = regmap_update_bits(chip->regmap,
					 chip->base + FAST_CHARGE_CURRENT_CFG,
					 FAST_CHARGE_CURRENT_SETTING_MASK,
					 current_ua / chip->match_data->fcc_step_ua);
		if (!ret)
			ret = regmap_write(chip->regmap, chip->base + BARK_BITE_WDOG_PET,
					   BARK_BITE_WDOG_PET_BIT);
		if (!ret)
			ret = pmi632_charge_enable(chip, true);
		if (ret)
			pmi632_charge_enable(chip, false);
	}

	if (was_full != chip->pmi632_full_latched)
		power_supply_changed(chip->chg_psy);

	if (READ_ONCE(chip->ready))
		mod_delayed_work(system_freezable_wq, &chip->pmi632_safety_work,
				 msecs_to_jiffies(1000));
out:
	mutex_unlock(&chip->pmi632_lock);
}

#endif /* QCOM_PMI632_POLICY_H */
