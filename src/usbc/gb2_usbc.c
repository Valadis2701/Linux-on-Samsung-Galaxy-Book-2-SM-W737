// SPDX-License-Identifier: GPL-2.0
/*
 * gb2_usbc.c - Samsung Galaxy Book2 (SM-W737) USB Type-C support.
 *
 * Two chips on GENI i2c bus 12 (i2c@a90000):
 *   - PTN36502 @0x1a - NXP Type-C USB3.1/DP redriver (orientation + mode)
 *   - S2MM005  @0x33 - Samsung Type-C PD controller
 *
 * The S2MM005 is a single-port PD controller.  It does NOT own the VBUS
 * path to the charger: that path is a separate "consumer switch" inside
 * the chip which has to be enabled with a vendor interface command on
 * every attach.  UEFI does exactly that (usbpwrctrl_s2mm005.efi,
 * "set Consumer Switch ON"), which is why charging works when the tablet
 * boots with the cable inserted and stops after a replug when the switch
 * is left open.
 *
 * Register protocol (from the Samsung Android reference driver):
 *   - 16-bit register address, MSB first, data little endian
 *   - interface commands: {0x03, cmd, arg...} written to REG_I2C_CMD 0x10
 *     * 0x01                 interrupt clear
 *     * 0x03 <pdo>           select PDO
 *     * 0x02 <state>         force PD state (0x13 = SNK_Select_Capability)
 *     * 0x91 00 01 00 02     consumer (VBUS to charger) switch ON
 *     * 0x91 00 00 00 00     consumer switch OFF
 *
 * Attach is reported in FUNC_STATE (0x20), not in the legacy CC latch:
 *   byte0   PD state (21 = SNK_Ready, 0 = detached)
 *   bits 8..10  CC1 plug state, bits 12..14 CC2 plug state
 *   bit 24  ATTACH_DONE, bit 25 IS_SOURCE, bit 26 IS_DFP
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/power_supply.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_mux.h>

/* ---------------- PTN36502 ---------------- */

#define PTN_CHIP_ID_REG		0x00
#define PTN_CHIP_ID		0x02
#define PTN_REVISION_REG	0x01
#define PTN_MODE_CTRL1_REG	0x0b
#define PTN_MODE_MASK		0x07
#define PTN_MODE_OFF		0x00
#define PTN_MODE_USB_ONLY	0x01
#define PTN_MODE_USB_DP		0x02
#define PTN_MODE_DP		0x03
#define PTN_ORIENT_REVERSE	BIT(5)

struct gb2_ptn {
	struct i2c_client *client;
	struct mutex lock;
	enum typec_orientation orientation;
	unsigned long mode;
	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;
};

static int gb2_ptn_write(struct gb2_ptn *ptn, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(ptn->client, reg, val);
}

static int gb2_ptn_read(struct gb2_ptn *ptn, u8 reg)
{
	return i2c_smbus_read_byte_data(ptn->client, reg);
}

static int gb2_ptn_apply(struct gb2_ptn *ptn)
{
	u8 val = 0;
	int ret;

	switch (ptn->mode) {
	case TYPEC_STATE_SAFE:
		val = PTN_MODE_OFF;
		break;
	case TYPEC_MODE_USB3:
	case TYPEC_MODE_USB2:
		val = PTN_MODE_USB_ONLY;
		break;
	default:
		val = PTN_MODE_USB_ONLY;
		break;
	}
	if (ptn->orientation == TYPEC_ORIENTATION_REVERSE)
		val |= PTN_ORIENT_REVERSE;

	if (gb2_ptn_read(ptn, PTN_MODE_CTRL1_REG) == val)
		return 0;

	ret = gb2_ptn_write(ptn, PTN_MODE_CTRL1_REG, val);
	if (!ret)
		dev_dbg(&ptn->client->dev, "mode=0x%02x (orientation=%d)\n",
			val, ptn->orientation);
	return ret;
}

static int gb2_ptn_switch_set(struct typec_switch_dev *sw,
			      enum typec_orientation orientation)
{
	struct gb2_ptn *ptn = typec_switch_get_drvdata(sw);
	int ret;

	mutex_lock(&ptn->lock);
	ptn->orientation = orientation;
	ret = gb2_ptn_apply(ptn);
	mutex_unlock(&ptn->lock);
	return ret;
}

static int gb2_ptn_mux_set(struct typec_mux_dev *mux,
			   struct typec_mux_state *state)
{
	struct gb2_ptn *ptn = typec_mux_get_drvdata(mux);
	int ret;

	mutex_lock(&ptn->lock);
	ptn->mode = state->mode;
	ret = gb2_ptn_apply(ptn);
	mutex_unlock(&ptn->lock);
	return ret;
}

static int gb2_ptn_probe(struct i2c_client *client)
{
	struct gb2_ptn *ptn;
	struct typec_switch_desc sw_desc = { };
	struct typec_mux_desc mux_desc = { };
	int id, rev;

	ptn = devm_kzalloc(&client->dev, sizeof(*ptn), GFP_KERNEL);
	if (!ptn)
		return -ENOMEM;
	ptn->client = client;
	mutex_init(&ptn->lock);
	ptn->orientation = TYPEC_ORIENTATION_NONE;
	i2c_set_clientdata(client, ptn);

	id = gb2_ptn_read(ptn, PTN_CHIP_ID_REG);
	rev = gb2_ptn_read(ptn, PTN_REVISION_REG);
	dev_info(&client->dev, "PTN36502 id=0x%02x rev=0x%02x\n", id, rev);
	if (id != PTN_CHIP_ID)
		dev_warn(&client->dev, "unexpected chip id\n");

	sw_desc.fwnode = dev_fwnode(&client->dev);
	sw_desc.set = gb2_ptn_switch_set;
	sw_desc.name = "ptn36502";
	sw_desc.drvdata = ptn;
	ptn->sw = typec_switch_register(&client->dev, &sw_desc);
	if (IS_ERR(ptn->sw)) {
		dev_err(&client->dev, "failed to register switch: %ld\n",
			PTR_ERR(ptn->sw));
		return PTR_ERR(ptn->sw);
	}

	mux_desc.fwnode = dev_fwnode(&client->dev);
	mux_desc.set = gb2_ptn_mux_set;
	mux_desc.name = "ptn36502";
	mux_desc.drvdata = ptn;
	ptn->mux = typec_mux_register(&client->dev, &mux_desc);
	if (IS_ERR(ptn->mux)) {
		dev_err(&client->dev, "failed to register mux: %ld\n",
			PTR_ERR(ptn->mux));
		typec_switch_unregister(ptn->sw);
		return PTR_ERR(ptn->mux);
	}

	dev_info(&client->dev, "PTN36502 registered (switch+mux)\n");
	return 0;
}

static void gb2_ptn_remove(struct i2c_client *client)
{
	struct gb2_ptn *ptn = i2c_get_clientdata(client);

	typec_mux_unregister(ptn->mux);
	typec_switch_unregister(ptn->sw);
}

/* ---------------- S2MM005 ---------------- */

#define S2MM005_REG_HW_VER	0x00
#define S2MM005_REG_SW_VER	0x08
#define S2MM005_REG_I2C_CMD	0x10
#define S2MM005_REG_FUNC_STATE	0x20
#define S2MM005_REG_MSG_IRQ	0x40
#define S2MM005_REG_TX_SINK	0x0220
#define S2MM005_REG_TX_REQUEST	0x0240
#define S2MM005_REG_RX_SRC	0x0260

#define S2MM005_MODE_INTERFACE	0x03
#define S2MM005_CMD_INT_CLEAR	0x01
#define S2MM005_CMD_PD_NEXT	0x02
#define S2MM005_CMD_PDO_SELECT	0x03
#define S2MM005_CMD_SWITCH	0x91

/* PD next-state used together with a PDO selection (UEFI/Android) */
#define S2MM005_PE_SNK_WAIT_CAPS	0x11
#define S2MM005_PE_SNK_SELECT_CAP	0x13

/* FUNC_STATE bits */
#define S2MM005_FS_ATTACH_DONE	BIT(24)
#define S2MM005_FS_IS_SOURCE	BIT(25)
#define S2MM005_FS_IS_DFP	BIT(26)

/* How often to poll FUNC_STATE */
#define GB2_POLL_MS		500

struct gb2_s2m {
	struct i2c_client *client;
	struct mutex lock;
	struct delayed_work poll;

	struct typec_port *port;
	struct typec_switch *sw;
	struct typec_mux *mux;
	struct regulator *vbus;		/* optional VBUS source switch */
	bool sink_seen;			/* sink attach since the last detach */
	unsigned long last_reset;	/* jiffies of the last flip reset */
	struct power_supply *psy;
	struct power_supply_desc psy_desc;
	char psy_name[16];
	unsigned int selected_ma;	/* current of the selected PDO */
	bool vbus_on;

	bool attached;
	bool sink;
	bool pd_selected;
	int pd_retry;
	int detach_cnt;
	u8 selected_pdo;
	unsigned int selected_mv;
	int slot;
	int verify_tries;
	unsigned long last_select;
	u32 last_fv;
	bool last_attach;
	bool last_src;
	unsigned int last_state;
};

/* ---------------- 16-bit Samsung transfer ---------------- */

static int gb2_s2m_write16(struct gb2_s2m *s, u16 reg, const u8 *buf, int len)
{
	struct i2c_client *c = s->client;
	struct i2c_msg m;
	u8 wb[16];
	int ret, tries = 3;

	if (len + 2 > sizeof(wb))
		return -EINVAL;
	wb[0] = reg >> 8;
	wb[1] = reg & 0xff;
	memcpy(&wb[2], buf, len);

	m.addr = c->addr;
	m.flags = 0;
	m.len = 2 + len;
	m.buf = wb;
	do {
		ret = i2c_transfer(c->adapter, &m, 1);
		if (ret == 1)
			return 0;
		usleep_range(1000, 2000);
	} while (--tries);
	return ret < 0 ? ret : -EIO;
}

static int gb2_s2m_read16(struct gb2_s2m *s, u16 reg, u8 *buf, int len)
{
	struct i2c_client *c = s->client;
	struct i2c_msg m[2];
	u8 wb[2] = { reg >> 8, reg & 0xff };
	int ret, tries = 3;

	m[0].addr = c->addr;
	m[0].flags = 0;
	m[0].len = 2;
	m[0].buf = wb;
	m[1].addr = c->addr;
	m[1].flags = I2C_M_RD;
	m[1].len = len;
	m[1].buf = buf;

	do {
		ret = i2c_transfer(c->adapter, m, 2);
		if (ret == 2)
			return 0;
		usleep_range(1000, 2000);
	} while (--tries);
	return ret < 0 ? ret : -EIO;
}

static int gb2_s2m_cmd(struct gb2_s2m *s, u8 cmd, u8 arg)
{
	u8 wd[3] = { S2MM005_MODE_INTERFACE, cmd, arg };

	return gb2_s2m_write16(s, S2MM005_REG_I2C_CMD, wd, sizeof(wd));
}

static int gb2_s2m_int_clear(struct gb2_s2m *s)
{
	u8 wd[1] = { S2MM005_CMD_INT_CLEAR };

	return gb2_s2m_write16(s, S2MM005_REG_I2C_CMD, wd, sizeof(wd));
}

/*
 * Consumer switch: connects the attached receptacle's VBUS to the charger
 * input.  UEFI sends this on every attach; without it the ISL9238 never
 * sees ACIN (ACOK stays 0) and the battery does not charge.
 */
static int gb2_s2m_consumer_switch(struct gb2_s2m *s, bool on)
{
	u8 wd[6] = {
		S2MM005_MODE_INTERFACE, S2MM005_CMD_SWITCH,
		0x00, on ? 0x01 : 0x00, 0x00, on ? 0x02 : 0x00,
	};

	return gb2_s2m_write16(s, S2MM005_REG_I2C_CMD, wd, sizeof(wd));
}

/* EmuEC's wake-up before a command: five one-byte reads of 0x0008 */
static void gb2_s2m_wake(struct gb2_s2m *s)
{
	u8 b;
	int i;

	for (i = 0; i < 5; i++)
		gb2_s2m_read16(s, 0x0008, &b, 1);
	udelay(10);
}

static int gb2_s2m_init_cmd(struct gb2_s2m *s, const u8 *cmd, int len)
{
	gb2_s2m_wake(s);
	return gb2_s2m_write16(s, S2MM005_REG_I2C_CMD, cmd, len);
}

/* EmuEC.sys FUN_1400168d8, which runs this on both controllers at start */
static int gb2_s2m_emuec_init(struct gb2_s2m *s)
{
	static const u8 lpm_init[] = { 0x0f, 0x0b };
	static const u8 soft_reset[] = { 0x02, 0x01, 0x1c, 0x10, 0x01 };
	static const u8 cfg[] = { 0x03, 0x08, 0x14 };
	static const u8 lpm_auto[] = { 0x0f, 0x06 };
	u8 status[48];
	int ret;

	ret = gb2_s2m_init_cmd(s, lpm_init, sizeof(lpm_init));
	if (ret)
		return ret;
	msleep(110);
	ret = gb2_s2m_init_cmd(s, soft_reset, sizeof(soft_reset));
	if (ret)
		return ret;
	msleep(100);
	ret = gb2_s2m_write16(s, S2MM005_REG_I2C_CMD, cfg, sizeof(cfg));
	if (!ret)
		ret = gb2_s2m_init_cmd(s, lpm_auto, sizeof(lpm_auto));
	if (!ret)
		ret = gb2_s2m_read16(s, 0x0030, status, sizeof(status));
	if (!ret)
		ret = gb2_s2m_int_clear(s);
	return ret;
}

/* Back to dual-role toggling after a detach (EmuEC.sys FUN_140014a38) */
static int gb2_s2m_drp_mode(struct gb2_s2m *s)
{
	u8 wd[5] = { 0x02, 0x01, 0x00, 0x50, 0x03 };

	return gb2_s2m_write16(s, S2MM005_REG_I2C_CMD, wd, sizeof(wd));
}

/* VBUS out from this port, if the board gives it a switch */
static void gb2_s2m_vbus(struct gb2_s2m *s, bool on)
{
	int ret;

	if (!s->vbus || s->vbus_on == on)
		return;
	ret = on ? regulator_enable(s->vbus) : regulator_disable(s->vbus);
	if (ret) {
		dev_err(&s->client->dev, "VBUS %s failed: %d\n",
			on ? "on" : "off", ret);
		return;
	}
	s->vbus_on = on;
	dev_info(&s->client->dev, "VBUS %s\n", on ? "on" : "off");
}

/*
 * Provider switch: our VBUS out on this receptacle. The command is
 * 03 91 P C 00 (P | C << 1) (EmuEC.sys FUN_140012fd0); the consumer switch
 * stays open while we provide.
 */
/* Bring-up A/B test: 1 = the old bytes (03 91 01 01 00 03 / 03 91 01 00 00 01) */
static bool provider_legacy;
module_param(provider_legacy, bool, 0644);
MODULE_PARM_DESC(provider_legacy, "use the pre-EmuEC provider switch bytes (testing)");

static int gb2_s2m_provider_switch(struct gb2_s2m *s, bool on)
{
	u8 wd[6] = {
		S2MM005_MODE_INTERFACE, S2MM005_CMD_SWITCH,
		on ? 0x01 : 0x00, 0x00, 0x00, on ? 0x01 : 0x00,
	};

	if (provider_legacy) {
		wd[2] = 0x01;
		wd[3] = on ? 0x01 : 0x00;
		wd[5] = on ? 0x03 : 0x01;
	}

	return gb2_s2m_write16(s, S2MM005_REG_I2C_CMD, wd, sizeof(wd));
}

/*
 * ---------------- charge path arbitration ----------------
 *
 * There is one ISL9237 and one VBUS input node, but two S2MM005 ports, each
 * with its own consumer switch.  Closing both switches ties two adapters
 * together on the charger input: with different contracts the higher voltage
 * source drives into the lower one.  The vendor EC keeps exactly one switch
 * closed and moves it around ("Consumer Switch Swap : Port1 -> Port2").
 *
 * Policy: the port with the highest negotiated voltage owns the path; on a
 * tie the current owner keeps it.  Order is always close-then-open, so the
 * two switches are never closed at the same time, not even briefly.
 */

#define GB2_MAX_PORTS	2

static DEFINE_MUTEX(gb2_path_lock);
static struct gb2_s2m *gb2_path_owner;
static struct gb2_s2m *gb2_ports[GB2_MAX_PORTS];

static void gb2_path_update(struct gb2_s2m *s)
{
	struct gb2_s2m *new_owner = NULL;
	unsigned int best = 0;
	int i;

	mutex_lock(&gb2_path_lock);

	if (s->slot < 0 || s->slot >= GB2_MAX_PORTS ||
	    gb2_ports[s->slot] != s) {
		mutex_unlock(&gb2_path_lock);
		return;
	}

	for (i = 0; i < GB2_MAX_PORTS; i++) {
		struct gb2_s2m *p = gb2_ports[i];

		if (!p || !p->attached || !p->sink)
			continue;
		if (!new_owner || p->selected_mv > best) {
			new_owner = p;
			best = p->selected_mv;
		}
	}

	/* do not churn when the current owner is as good as the winner */
	if (new_owner && new_owner != gb2_path_owner && gb2_path_owner &&
	    gb2_path_owner->attached && gb2_path_owner->sink &&
	    gb2_path_owner->selected_mv == best)
		new_owner = gb2_path_owner;

	if (new_owner == gb2_path_owner) {
		mutex_unlock(&gb2_path_lock);
		return;
	}

	/* close first: never leave both inputs tied to the charger at once */
	if (gb2_path_owner) {
		gb2_s2m_consumer_switch(gb2_path_owner, false);
		dev_info(&gb2_path_owner->client->dev,
			 "charge path released\n");
	}

	if (gb2_path_owner)
		power_supply_changed(gb2_path_owner->psy);
	gb2_path_owner = new_owner;
	if (new_owner)
		power_supply_changed(new_owner->psy);

	if (new_owner) {
		if (gb2_s2m_consumer_switch(new_owner, true))
			dev_warn(&new_owner->client->dev,
				 "consumer switch ON failed\n");
		else
			dev_info(&new_owner->client->dev,
				 "consumer switch ON (%u mV, owns charge path)\n",
				 new_owner->selected_mv);
	}

	mutex_unlock(&gb2_path_lock);
}

/*
 * Choose a fixed PDO no higher than 12 V (same policy as the Samsung
 * reference: AVAILABLE_VOLTAGE 12000), then push the PE to the select
 * state.  The pair must be written back to back like UEFI does; the old
 * driver used 0x59 as the second argument, which left the PE confused.
 */
/* ---------------- power supply ---------------- */

static const enum power_supply_property gb2_s2m_psy_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int gb2_s2m_psy_get(struct power_supply *psy,
			   enum power_supply_property psp,
			   union power_supply_propval *val)
{
	struct gb2_s2m *s = power_supply_get_drvdata(psy);
	bool online, pd;

	mutex_lock(&gb2_path_lock);
	online = gb2_path_owner == s;
	pd = online && s->selected_mv && s->selected_ma;
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = online ? (pd ? s->selected_mv : 5000) * 1000 : 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = online ? (pd ? s->selected_ma : 500) * 1000 : 0;
		break;
	default:
		mutex_unlock(&gb2_path_lock);
		return -EINVAL;
	}
	mutex_unlock(&gb2_path_lock);
	return 0;
}

static int gb2_s2m_select_pdo(struct gb2_s2m *s, unsigned int *selected,
			      unsigned int *mv)
{
	u8 capa[32];
	u32 hdr, pdo;
	int ret, nobjs, i, best = 0;
	unsigned int best_mv = 0, best_ma = 0;

	ret = gb2_s2m_read16(s, S2MM005_REG_RX_SRC, capa, sizeof(capa));
	if (ret)
		return ret;

	hdr = capa[0] | capa[1] << 8 | capa[2] << 16 | capa[3] << 24;
	if ((hdr & 0x1f) != 1)		/* Source_Capabilities */
		return -EAGAIN;
	nobjs = (hdr >> 12) & 0x7;
	if (!nobjs)
		return -EAGAIN;

	for (i = 0; i < nobjs; i++) {
		pdo = capa[4 + i * 4] | capa[5 + i * 4] << 8 |
		      capa[6 + i * 4] << 16 | capa[7 + i * 4] << 24;
		if ((pdo >> 30) != 0)	/* fixed supply only */
			continue;
		if (((pdo >> 10) & 0x3ff) * 50 <= 12000) {
			unsigned int mv = ((pdo >> 10) & 0x3ff) * 50;

			if (mv > best_mv) {
				best_mv = mv;
				best_ma = (pdo & 0x3ff) * 10;
				best = i + 1;
			}
		}
	}
	if (!best)			/* fall back to the first PDO */
		best = 1;

	ret = gb2_s2m_cmd(s, S2MM005_CMD_PDO_SELECT, best);
	if (ret)
		return ret;
	usleep_range(5000, 8000);
	ret = gb2_s2m_cmd(s, S2MM005_CMD_PD_NEXT,
			  S2MM005_PE_SNK_SELECT_CAP);
	if (ret)
		return ret;

	*selected = best;
	if (mv)
		*mv = best_mv;
	s->selected_ma = best_ma;
	return 0;
}

static enum typec_orientation gb2_s2m_orientation(u32 fv)
{
	unsigned int cc1 = (fv >> 8) & 0x7;
	unsigned int cc2 = (fv >> 12) & 0x7;

	/* The active CC line reports the detected Rp level (1..3);
	 * the idle one is either open (0) or Ra (4).
	 */
	if (cc1 >= 1 && cc1 <= 3 && (cc2 == 0 || cc2 == 4))
		return TYPEC_ORIENTATION_NORMAL;
	if (cc2 >= 1 && cc2 <= 3 && (cc1 == 0 || cc1 == 4))
		return TYPEC_ORIENTATION_REVERSE;
	return TYPEC_ORIENTATION_NONE;
}

static void gb2_s2m_refresh(struct gb2_s2m *s)
{
	struct device *dev = &s->client->dev;
	struct typec_mux_state mux_state = { };
	enum typec_orientation orient;
	bool attach, is_src, sink;
	u32 fv;
	int ret;

	ret = gb2_s2m_read16(s, S2MM005_REG_FUNC_STATE, (u8 *)&fv, 4);
	if (ret) {
		if (s->last_fv != 0xffffffff)
			dev_dbg(dev, "FUNC_STATE read failed: %d\n", ret);
		s->last_fv = 0xffffffff;
		return;
	}

	attach = !!(fv & S2MM005_FS_ATTACH_DONE);
	is_src = !!(fv & S2MM005_FS_IS_SOURCE);
	sink = attach && !is_src;
	orient = gb2_s2m_orientation(fv);

	/*
	 * While detached the chip keeps alternating FUNC_STATE between two
	 * values (CC scan), so a plain "value changed" test floods the log
	 * at twice the poll rate.  Log only real attach / role transitions.
	 */
	if (fv != s->last_fv) {
		s->last_fv = fv;
		if (attach != s->last_attach || is_src != s->last_src) {
			dev_info(dev,
				 "FUNC=0x%08x state=%u CC1=%u CC2=%u attach=%u src=%u dfp=%u rp=%u\n",
				 fv, fv & 0xff, (fv >> 8) & 7, (fv >> 12) & 7,
				 attach, is_src, !!(fv & S2MM005_FS_IS_DFP),
				 (fv >> 27) & 3);
			s->last_attach = attach;
			s->last_src = is_src;
		}
	}

	if (sink) {
		s->detach_cnt = 0;
		if (!s->attached || !s->sink) {
			gb2_s2m_vbus(s, false);
			s->sink_seen = true;
			s->attached = true;
			s->sink = true;
			s->pd_selected = false;
			s->pd_retry = 0;
			s->verify_tries = 0;

			if (s->sw && orient != TYPEC_ORIENTATION_NONE)
				typec_switch_set(s->sw, orient);
			if (s->mux) {
				mux_state.mode = TYPEC_MODE_USB2;
				typec_mux_set(s->mux, &mux_state);
			}

			typec_set_data_role(s->port, TYPEC_DEVICE);
			typec_set_pwr_role(s->port, TYPEC_SINK);
			if (orient != TYPEC_ORIENTATION_NONE)
				typec_set_orientation(s->port, orient);
			typec_set_mode(s->port, TYPEC_MODE_USB2);

			/* Take the (single) charge path right away, so that a
			 * 5 V default contract can already charge.  The
			 * arbitration keeps the switch open if the other port
			 * already owns the path and has a better contract.
			 */
			gb2_path_update(s);
		}

		if (!s->pd_selected) {
			unsigned int pdo = 0, mv = 0;

			ret = gb2_s2m_select_pdo(s, &pdo, &mv);
			if (!ret) {
				s->pd_selected = true;
				s->selected_pdo = pdo;
				s->selected_mv = mv;
				s->verify_tries = 0;
				s->last_select = jiffies;
				dev_info(dev, "PDO %u selected (%u mV)\n",
					 pdo, mv);
				gb2_path_update(s);
			} else if (++s->pd_retry >= 10) {
				s->pd_selected = true;
				dev_info(dev,
					 "no source caps (%d), keeping default contract\n",
					 ret);
			}
		} else if (s->verify_tries < 3 &&
			   time_after(jiffies,
				      s->last_select + msecs_to_jiffies(1200))) {
			u8 req[8];
			u32 rdo;
			unsigned int obj;

			if (!gb2_s2m_read16(s, S2MM005_REG_TX_REQUEST,
					    req, sizeof(req))) {
				rdo = req[4] | req[5] << 8 | req[6] << 16 |
				      req[7] << 24;
				obj = (rdo >> 28) & 0xf;
				if (obj == s->selected_pdo) {
					s->verify_tries = 3;
					dev_info(dev, "contract confirmed on PDO %u\n",
						 obj);
				} else {
					unsigned int pdo = 0, mv = 0;

					if (!gb2_s2m_select_pdo(s, &pdo, &mv)) {
						s->selected_pdo = pdo;
						s->selected_mv = mv;
						s->last_select = jiffies;
						s->verify_tries++;
						dev_info(dev,
							 "contract on PDO %u, re-selected %u (%u mV)\n",
							 obj, pdo, mv);
						gb2_path_update(s);
					}
				}
			}
		}
	} else if (attach && is_src) {
		s->detach_cnt = 0;
		if (s->vbus && s->sink && !s->selected_mv && s->sink_seen &&
		    time_after(jiffies, s->last_reset + 10 * HZ)) {
			s->last_reset = jiffies;
			s->sink_seen = false;
			dev_info(dev, "sink-to-source flip without contract: resetting\n");
			gb2_s2m_vbus(s, false);
			s->attached = false;
			s->sink = false;
			gb2_path_update(s);	/* drop the charge path first */
			gb2_s2m_emuec_init(s);
			return;
		}
		if (!s->attached || s->sink) {
			s->attached = true;
			s->sink = false;
			s->pd_selected = false;

			if (s->sw && orient != TYPEC_ORIENTATION_NONE)
				typec_switch_set(s->sw, orient);
			typec_set_data_role(s->port, TYPEC_HOST);
			typec_set_pwr_role(s->port, TYPEC_SOURCE);
			typec_set_mode(s->port, TYPEC_MODE_USB2);
			gb2_s2m_provider_switch(s, true);
			gb2_s2m_vbus(s, true);
			/* no longer a sink: give the charge path away */
			gb2_path_update(s);
		}
	} else if (s->attached) {
		s->sink_seen = false;
		s->attached = false;
		s->sink = false;
		s->pd_selected = false;
		s->pd_retry = 0;
		s->selected_mv = 0;

		/* closes our switch if we owned the path, hands it over */
		gb2_path_update(s);
		gb2_s2m_provider_switch(s, false);
		gb2_s2m_vbus(s, false);
		gb2_s2m_drp_mode(s);
		if (s->mux) {
			mux_state.mode = TYPEC_STATE_SAFE;
			typec_mux_set(s->mux, &mux_state);
		}
		typec_set_mode(s->port, TYPEC_STATE_SAFE);
		typec_set_pwr_role(s->port, TYPEC_SINK);
		typec_set_data_role(s->port, TYPEC_DEVICE);
		dev_info(dev, "detached\n");
	}
}

static void gb2_s2m_poll(struct work_struct *work)
{
	struct gb2_s2m *s = container_of(to_delayed_work(work),
					 struct gb2_s2m, poll);

	mutex_lock(&s->lock);
	gb2_s2m_refresh(s);
	mutex_unlock(&s->lock);

	schedule_delayed_work(&s->poll, msecs_to_jiffies(GB2_POLL_MS));
}

static int gb2_s2m_try_role(struct typec_port *port, int role)
{
	if (role < TYPEC_NO_PREFERRED_ROLE || role > TYPEC_SOURCE)
		return -EINVAL;
	return 0;
}

static int gb2_s2m_dr_set(struct typec_port *port, enum typec_data_role role)
{
	return 0;
}

static int gb2_s2m_pr_set(struct typec_port *port, enum typec_role role)
{
	return 0;
}

static const struct typec_operations gb2_s2m_ops = {
	.try_role = gb2_s2m_try_role,
	.dr_set = gb2_s2m_dr_set,
	.pr_set = gb2_s2m_pr_set,
};

static int gb2_s2m_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_capability cap = { };
	struct gb2_s2m *s;
	u8 ver[8];
	int i, ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->client = client;
	s->slot = -1;
	mutex_init(&s->lock);
	i2c_set_clientdata(client, s);

	s->vbus = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(s->vbus)) {
		if (PTR_ERR(s->vbus) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(s->vbus),
					     "cannot get VBUS switch\n");
		s->vbus = NULL;
	}

	mutex_lock(&gb2_path_lock);
	for (i = 0; i < GB2_MAX_PORTS; i++)
		if (!gb2_ports[i])
			break;
	if (i == GB2_MAX_PORTS) {
		mutex_unlock(&gb2_path_lock);
		dev_err(dev, "no free port slot\n");
		return -ENOSPC;
	}
	s->slot = i;
	gb2_ports[i] = s;
	mutex_unlock(&gb2_path_lock);

	ret = gb2_s2m_read16(s, S2MM005_REG_HW_VER, ver, sizeof(ver));
	if (ret) {
		dev_err(dev, "failed to read version: %d\n", ret);
		return ret;
	}
	dev_info(dev, "S2MM005 hw=%u.%u.%u.%u sw=%u.%u.%u.%u\n",
		 ver[0], ver[1], ver[2], ver[3],
		 ver[4], ver[5], ver[6], ver[7]);

	gb2_s2m_int_clear(s);

	/* Only where we switch VBUS ourselves: the other port feeds the boot disk */
	if (s->vbus) {
		ret = gb2_s2m_emuec_init(s);
		if (ret)
			dev_warn(dev, "EmuEC init failed: %d\n", ret);
		else
			dev_info(dev, "EmuEC init done\n");
	}

	cap.type = TYPEC_PORT_DRP;
	cap.data = TYPEC_PORT_DRD;
	cap.revision = 0x0120;
	cap.pd_revision = 0x0300;
	cap.prefer_role = TYPEC_NO_PREFERRED_ROLE;
	cap.orientation_aware = 1;
	cap.fwnode = dev_fwnode(dev);
	cap.ops = &gb2_s2m_ops;
	cap.driver_data = s;

	s->sw = fwnode_typec_switch_get(dev_fwnode(dev));
	if (IS_ERR(s->sw)) {
		dev_dbg(dev, "no typec switch: %ld\n", PTR_ERR(s->sw));
		s->sw = NULL;
	}
	s->mux = fwnode_typec_mux_get(dev_fwnode(dev));
	if (IS_ERR(s->mux)) {
		dev_dbg(dev, "no typec mux: %ld\n", PTR_ERR(s->mux));
		s->mux = NULL;
	}

	s->port = typec_register_port(dev, &cap);
	if (IS_ERR(s->port)) {
		ret = PTR_ERR(s->port);
		s->port = NULL;
		dev_err(dev, "port registration failed: %d\n", ret);
		return ret;
	}

	{
		struct power_supply_config psy_cfg = { .drv_data = s };

		snprintf(s->psy_name, sizeof(s->psy_name), "gb2-typec%d", s->slot);
		s->psy_desc.name = s->psy_name;
		s->psy_desc.type = POWER_SUPPLY_TYPE_USB;
		s->psy_desc.properties = gb2_s2m_psy_props;
		s->psy_desc.num_properties = ARRAY_SIZE(gb2_s2m_psy_props);
		s->psy_desc.get_property = gb2_s2m_psy_get;
		s->psy = devm_power_supply_register(dev, &s->psy_desc, &psy_cfg);
		if (IS_ERR(s->psy)) {
			ret = PTR_ERR(s->psy);
			s->psy = NULL;
			typec_unregister_port(s->port);
			s->port = NULL;
			mutex_lock(&gb2_path_lock);
			gb2_ports[s->slot] = NULL;
			mutex_unlock(&gb2_path_lock);
			return dev_err_probe(dev, ret, "power supply registration failed\n");
		}
	}

	mutex_lock(&s->lock);
	gb2_s2m_refresh(s);
	mutex_unlock(&s->lock);

	INIT_DELAYED_WORK(&s->poll, gb2_s2m_poll);
	schedule_delayed_work(&s->poll, msecs_to_jiffies(GB2_POLL_MS));
	dev_info(dev, "registered typec port\n");
	return 0;
}

static void gb2_s2m_remove(struct i2c_client *client)
{
	struct gb2_s2m *s = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&s->poll);

	mutex_lock(&gb2_path_lock);
	if (gb2_path_owner == s) {
		gb2_s2m_consumer_switch(s, false);
		gb2_path_owner = NULL;
	}
	if (s->slot >= 0 && s->slot < GB2_MAX_PORTS &&
	    gb2_ports[s->slot] == s)
		gb2_ports[s->slot] = NULL;
	mutex_unlock(&gb2_path_lock);

	if (s->attached)
		gb2_s2m_provider_switch(s, false);
	gb2_s2m_vbus(s, false);
	if (s->port)
		typec_unregister_port(s->port);
	/* The typec port owns the switch/mux references it took at
	 * registration time; putting ours here as well crashes the core.
	 */
}

/* ---------------- module ---------------- */

static const struct of_device_id gb2_ptn_of_match[] = {
	{ .compatible = "nxp,ptn36502" },
	{ }
};
MODULE_DEVICE_TABLE(of, gb2_ptn_of_match);

static const struct i2c_device_id gb2_ptn_id[] = {
	{ "ptn36502", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gb2_ptn_id);

static struct i2c_driver gb2_ptn_driver = {
	.driver = {
		.name = "gb2-ptn36502",
		.of_match_table = gb2_ptn_of_match,
	},
	.probe = gb2_ptn_probe,
	.remove = gb2_ptn_remove,
	.id_table = gb2_ptn_id,
};

static const struct of_device_id gb2_s2m_of_match[] = {
	{ .compatible = "siliconmitus,s2mm005" },
	{ }
};
MODULE_DEVICE_TABLE(of, gb2_s2m_of_match);

static const struct i2c_device_id gb2_s2m_id[] = {
	{ "s2mm005", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gb2_s2m_id);

static struct i2c_driver gb2_s2m_driver = {
	.driver = {
		.name = "gb2-s2mm005",
		.of_match_table = gb2_s2m_of_match,
	},
	.probe = gb2_s2m_probe,
	.remove = gb2_s2m_remove,
	.id_table = gb2_s2m_id,
};

static int __init gb2_usbc_init(void)
{
	int ret;

	ret = i2c_add_driver(&gb2_ptn_driver);
	if (ret)
		return ret;
	ret = i2c_add_driver(&gb2_s2m_driver);
	if (ret) {
		i2c_del_driver(&gb2_ptn_driver);
		return ret;
	}
	return 0;
}

static void __exit gb2_usbc_exit(void)
{
	i2c_del_driver(&gb2_s2m_driver);
	i2c_del_driver(&gb2_ptn_driver);
}

module_init(gb2_usbc_init);
module_exit(gb2_usbc_exit);

MODULE_DESCRIPTION("Galaxy Book2 USB Type-C (S2MM005 + PTN36502)");
MODULE_LICENSE("GPL");
