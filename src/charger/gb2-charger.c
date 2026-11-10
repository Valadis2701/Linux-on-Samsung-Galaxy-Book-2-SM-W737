// SPDX-License-Identifier: GPL-2.0
/*
 * gb2-charger.c - Samsung Galaxy Book2 (SM-W737) charging for Linux
 *
 * The ISL9238 buck-boost charger (I2C 0x09 on the GENI bus at 0x089c000)
 * charges only while ChargeCurrentLimit (0x14) is non-zero, and it drops
 * that setting to 0 about a minute after the last host write (SMBus
 * watchdog). Under Windows the embedded controller keeps it programmed;
 * under Linux nobody does, so the battery never charges.
 *
 * Every kick_ms this driver decides a charge current and writes it, which
 * also feeds the watchdog:
 *
 *   - 0 without an adapter (ACOK, Information2 bit 14),
 *   - 0 when the pack is at or above full_pct, until it drops 2 % below,
 *   - 0 outside 0..45 degC pack temperature,
 *   - 0 when the pack asks for a lower charge voltage than the charger is
 *     set to (MaxSystemVoltage, 0x15, left as the firmware set it), or when
 *     its request cannot be read,
 *   - otherwise min(charge_ma, the current the pack requests).
 *
 * The pack's requests are its Smart Battery ChargingCurrent and
 * ChargingVoltage words, read directly at 0x0b on the same bus because
 * sbs-battery does not export them.
 *
 * The adapter current limit (AdapterCurrentLimit1/2, 0x3F/0x3B) is set to
 * the current of the USB-C contract on the port that owns the charge path,
 * as gb2_usbc reports it ("gb2-typec<n>" power supplies), up to 3 A. The
 * chip reloads its PROG default (476 mA here) on every plug-in, so it is
 * rewritten on every refresh. Encoding assumes the reference 20 mOhm
 * adapter sense resistor: at the 476 mA default a 12 V adapter left the
 * battery about 0.7 W, which fits 20 mOhm and not 10.
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define ISL9238_CHARGE_CURRENT		0x14
#define ISL9238_MAX_SYS_VOLTAGE		0x15
#define ISL9238_ADAPTER_CURRENT2	0x3b
#define ISL9238_ADAPTER_CURRENT1	0x3f
#define ISL9238_INFO1			0x3a
#define ISL9238_INFO1_LOOP		GENMASK(14, 13)
#define ISL9238_INFO2			0x4d
#define ISL9238_INFO2_ACOK		BIT(14)
#define ISL9238_MANUFACTURER_ID		0xfe
#define ISL9238_DEVICE_ID		0xff

/* 4 mA steps in bits 12:2 with the 10 mOhm battery sense resistor */
#define ISL9238_CURRENT_MASK		0x1ffc

#define SBS_ADDR			0x0b
#define SBS_CHARGING_CURRENT		0x14	/* mA */
#define SBS_CHARGING_VOLTAGE		0x15	/* mV */

#define GB2_CHG_MAX_MA			2000
#define GB2_CHG_INPUT_MAX_MA		3000
#define GB2_CHG_TYPEC_PORTS		2
#define GB2_CHG_TEMP_MIN		0	/* 0.1 degC */
#define GB2_CHG_TEMP_MAX		450
#define GB2_CHG_RESUME_DROP		2	/* % below full_pct */

static unsigned int kick_ms = 10000;
module_param(kick_ms, uint, 0644);
MODULE_PARM_DESC(kick_ms, "charger refresh interval in ms (default 10000)");

static unsigned int charge_ma = 2000;
module_param(charge_ma, uint, 0644);
MODULE_PARM_DESC(charge_ma, "charge current in mA (default 2000, max 2000)");

static unsigned int full_pct = 96;
module_param(full_pct, uint, 0644);
MODULE_PARM_DESC(full_pct, "stop charging at this capacity (default 96)");

static char *battery_name = "sbs-7-000b";
module_param(battery_name, charp, 0644);
MODULE_PARM_DESC(battery_name, "power_supply name of the SBS battery");

struct gb2_chg {
	struct i2c_client *client;
	struct delayed_work kick;
	struct power_supply *battery;
	int charge_mv;		/* MaxSystemVoltage as found at probe */
	int set_ma;		/* last current written, -1 before the first */
	int input_ma;		/* last adapter limit written, 0 for none */
	int loop;		/* last active control loop logged */
	bool full;
	const char *reason;
};

static int gb2_chg_battery_int(struct gb2_chg *chg, enum power_supply_property psp,
			       int *val)
{
	union power_supply_propval pv;
	int ret;

	ret = power_supply_get_property(chg->battery, psp, &pv);
	if (!ret)
		*val = pv.intval;
	return ret;
}

/* A Smart Battery word from the pack; sbs-battery owns the client */
static int gb2_chg_sbs_word(struct gb2_chg *chg, u8 reg)
{
	union i2c_smbus_data data;
	int ret;

	ret = i2c_smbus_xfer(chg->client->adapter, SBS_ADDR, 0, I2C_SMBUS_READ,
			     reg, I2C_SMBUS_WORD_DATA, &data);
	return ret < 0 ? ret : data.word;
}

/* Current of the USB-C contract that feeds the charger, 0 if unknown */
static int gb2_chg_contract_ma(void)
{
	union power_supply_propval online, ua;
	char name[16];
	int i, ma = 0;

	for (i = 0; i < GB2_CHG_TYPEC_PORTS; i++) {
		struct power_supply *psy;

		snprintf(name, sizeof(name), "gb2-typec%d", i);
		psy = power_supply_get_by_name(name);
		if (!psy)
			continue;
		if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &online) &&
		    online.intval &&
		    !power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_MAX, &ua))
			ma = ua.intval / 1000;
		power_supply_put(psy);
		if (ma)
			break;
	}
	return min(ma, GB2_CHG_INPUT_MAX_MA);
}

/* The charge current to program now, with the reason when it is 0 */
static int gb2_chg_target_ma(struct gb2_chg *chg)
{
	int info2, cap, temp, req_ma, req_mv, ma;

	info2 = i2c_smbus_read_word_data(chg->client, ISL9238_INFO2);
	if (info2 < 0) {
		chg->reason = "charger not answering";
		return 0;
	}
	if (!(info2 & ISL9238_INFO2_ACOK)) {
		chg->reason = "no adapter";
		return 0;
	}

	if (!chg->battery)
		chg->battery = power_supply_get_by_name(battery_name);
	if (!chg->battery) {
		chg->reason = "no battery";
		return 0;
	}

	if (gb2_chg_battery_int(chg, POWER_SUPPLY_PROP_CAPACITY, &cap)) {
		chg->reason = "battery capacity unknown";
		return 0;
	}
	if (cap >= full_pct)
		chg->full = true;
	else if (cap <= (int)full_pct - GB2_CHG_RESUME_DROP)
		chg->full = false;
	if (chg->full) {
		chg->reason = "battery full";
		return 0;
	}

	if (gb2_chg_battery_int(chg, POWER_SUPPLY_PROP_TEMP, &temp) ||
	    temp < GB2_CHG_TEMP_MIN || temp > GB2_CHG_TEMP_MAX) {
		chg->reason = "battery temperature out of range or unknown";
		return 0;
	}

	/* The pack's own request: never charge to a higher voltage */
	req_mv = gb2_chg_sbs_word(chg, SBS_CHARGING_VOLTAGE);
	req_ma = gb2_chg_sbs_word(chg, SBS_CHARGING_CURRENT);
	if (req_mv < 0 || req_ma < 0) {
		chg->reason = "battery charge request unreadable";
		return 0;
	}
	if (req_mv < chg->charge_mv) {
		chg->reason = "battery asks for a lower charge voltage";
		return 0;
	}

	ma = min3((int)charge_ma, GB2_CHG_MAX_MA, req_ma);
	if (!ma)
		chg->reason = "battery requests no current";
	return ma;
}

static void gb2_chg_kick_work(struct work_struct *work)
{
	struct gb2_chg *chg = container_of(to_delayed_work(work),
					   struct gb2_chg, kick);
	struct device *dev = &chg->client->dev;
	static const char * const loops[] = {
		"charge voltage", "charge current", "adapter current", "input voltage"
	};
	int ma = gb2_chg_target_ma(chg);
	int input = gb2_chg_contract_ma();	/* 0 without an adapter port */
	int info1;

	/* Reloaded from PROG on every plug-in: write it each time */
	if (input) {
		i2c_smbus_write_word_data(chg->client, ISL9238_ADAPTER_CURRENT1,
					  input & ISL9238_CURRENT_MASK);
		i2c_smbus_write_word_data(chg->client, ISL9238_ADAPTER_CURRENT2,
					  input & ISL9238_CURRENT_MASK);
	}
	if (input != chg->input_ma) {
		if (input)
			dev_info(dev, "adapter limit %d mA (USB-C contract)\n", input);
		chg->input_ma = input;
	}

	/* Both writes feed the charger's watchdog */
	i2c_smbus_write_word_data(chg->client, ISL9238_MAX_SYS_VOLTAGE,
				  chg->charge_mv);
	i2c_smbus_write_word_data(chg->client, ISL9238_CHARGE_CURRENT,
				  ma & ISL9238_CURRENT_MASK);

	if (ma != chg->set_ma) {
		if (ma)
			dev_info(dev, "charging at %d mA to %d mV\n", ma,
				 chg->charge_mv);
		else
			dev_info(dev, "not charging: %s\n", chg->reason);
		chg->set_ma = ma;
	}

	/* Which limit is in charge right now (CC, CV, adapter, input V) */
	info1 = i2c_smbus_read_word_data(chg->client, ISL9238_INFO1);
	if (ma && info1 >= 0) {
		int loop = FIELD_GET(ISL9238_INFO1_LOOP, info1);

		if (loop != chg->loop) {
			dev_info(dev, "limited by %s\n", loops[loop]);
			chg->loop = loop;
		}
	} else {
		chg->loop = -1;
	}

	schedule_delayed_work(&chg->kick, msecs_to_jiffies(kick_ms));
}

static int gb2_chg_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gb2_chg *chg;
	int mfr, id, mv;

	mfr = i2c_smbus_read_word_data(client, ISL9238_MANUFACTURER_ID);
	id = i2c_smbus_read_word_data(client, ISL9238_DEVICE_ID);
	if (mfr < 0 || id < 0)
		return dev_err_probe(dev, -ENODEV, "charger not responding\n");
	if ((mfr & 0xff) != 0x49 || (id & 0xff) != 0x0c)
		return dev_err_probe(dev, -ENODEV,
				     "not an ISL9238 (mfr 0x%02x id 0x%02x)\n",
				     mfr & 0xff, id & 0xff);

	/* Keep the firmware's charge voltage; refuse anything implausible */
	mv = i2c_smbus_read_word_data(client, ISL9238_MAX_SYS_VOLTAGE);
	if (mv < 0)
		return dev_err_probe(dev, mv, "cannot read charge voltage\n");
	if (mv < 8000 || mv > 8800)
		return dev_err_probe(dev, -EINVAL,
				     "unexpected charge voltage %d mV\n", mv);

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->client = client;
	chg->charge_mv = mv;
	chg->set_ma = -1;
	chg->loop = -1;
	i2c_set_clientdata(client, chg);

	dev_info(dev, "ISL9238 at 0x%02x, charge voltage %d mV, up to %u mA\n",
		 client->addr, mv, min_t(unsigned int, charge_ma, GB2_CHG_MAX_MA));

	INIT_DELAYED_WORK(&chg->kick, gb2_chg_kick_work);
	schedule_delayed_work(&chg->kick, 0);
	return 0;
}

static void gb2_chg_remove(struct i2c_client *client)
{
	struct gb2_chg *chg = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&chg->kick);
	i2c_smbus_write_word_data(client, ISL9238_CHARGE_CURRENT, 0);
	if (chg->battery)
		power_supply_put(chg->battery);
}

static const struct of_device_id gb2_chg_of_match[] = {
	{ .compatible = "gb2,sbs-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, gb2_chg_of_match);

static struct i2c_driver gb2_chg_driver = {
	.driver = {
		.name = "gb2-sbs-charger",
		.of_match_table = gb2_chg_of_match,
	},
	.probe = gb2_chg_probe,
	.remove = gb2_chg_remove,
};
module_i2c_driver(gb2_chg_driver);

MODULE_DESCRIPTION("Samsung Galaxy Book2 (SM-W737) ISL9238 charging");
MODULE_LICENSE("GPL");
