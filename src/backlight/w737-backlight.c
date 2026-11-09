// SPDX-License-Identifier: GPL-2.0
/*
 * w737-backlight — expose the Galaxy Book2 (SM-W737) internal AMOLED panel as a
 * standard backlight class device, so GNOME / logind / UPower drive it with the
 * normal brightness slider.
 *
 * The panel has no PWM and no backlight controller: brightness is a TCON
 * register table sent over the DP AUX channel.  Reimplementing that engine in
 * kernel space is unnecessary, so this driver is a thin shim — it exposes
 * /sys/class/backlight/w737/ and, on change, runs
 *
 *      /usr/local/sbin/w737-brightness-set <level>
 *
 * which owns the AUX protocol, the calibration tables and the persisted level.
 * Writes are coalesced so dragging the slider does not fork a process per tick.
 */

#include <linux/backlight.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/umh.h>
#include <linux/workqueue.h>

#define DRV_NAME	"w737-backlight"
#define HELPER		"/usr/local/sbin/w737-brightness-set"
#define MAX_LEVEL	100
#define COALESCE_MS	15

struct w737_bl {
	struct backlight_device	*bd;
	struct delayed_work	work;
	struct mutex		lock;		/* protects level/argv below */
	int			level;
	char			level_str[8];
	char			*argv[3];
};

static void w737_bl_apply(struct work_struct *work)
{
	struct w737_bl *bl = container_of(to_delayed_work(work),
					  struct w737_bl, work);
	char *envp[] = {
		"HOME=/",
		"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
		NULL
	};
	int ret;

	mutex_lock(&bl->lock);
	snprintf(bl->level_str, sizeof(bl->level_str), "%d", bl->level);
	mutex_unlock(&bl->lock);

	/* UMH_WAIT_PROC so we get the helper's exit status, not just exec's. */
	ret = call_usermodehelper(bl->argv[0], bl->argv, envp, UMH_WAIT_PROC);
	if (ret)
		pr_warn(DRV_NAME ": helper failed for level %s: %d\n",
			bl->level_str, ret);
	else
		pr_info(DRV_NAME ": level %s applied\n", bl->level_str);
}

static int w737_bl_update(struct backlight_device *bd)
{
	struct w737_bl *bl = bl_get_data(bd);
	int level = bd->props.brightness;

	if (level < 0)
		level = 0;
	if (level > MAX_LEVEL)
		level = MAX_LEVEL;

	mutex_lock(&bl->lock);
	if (level == bl->level) {
		mutex_unlock(&bl->lock);
		return 0;
	}
	bl->level = level;
	mutex_unlock(&bl->lock);

	schedule_delayed_work(&bl->work,
			      msecs_to_jiffies(COALESCE_MS));
	return 0;
}

static const struct backlight_ops w737_bl_ops = {
	.update_status	= w737_bl_update,
};

static int w737_bl_probe(struct platform_device *pdev)
{
	struct backlight_properties props;
	struct backlight_device *bd;
	struct w737_bl *bl;

	bl = devm_kzalloc(&pdev->dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;

	mutex_init(&bl->lock);
	INIT_DELAYED_WORK(&bl->work, w737_bl_apply);

	bl->argv[0] = HELPER;
	bl->argv[1] = bl->level_str;
	bl->argv[2] = NULL;

	/*
	 * Logind hands us the saved level at session start; until then report a
	 * mid value so the slider is usable immediately.
	 */
	bl->level = 50;
	snprintf(bl->level_str, sizeof(bl->level_str), "%d", bl->level);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = MAX_LEVEL;
	props.brightness = bl->level;
	props.power = FB_BLANK_UNBLANK;

	bd = devm_backlight_device_register(&pdev->dev, "w737", &pdev->dev, bl,
					    &w737_bl_ops, &props);
	if (IS_ERR(bd))
		return dev_err_probe(&pdev->dev, PTR_ERR(bd),
				     "failed to register backlight\n");

	bl->bd = bd;
	platform_set_drvdata(pdev, bl);

	dev_info(&pdev->dev,
		 "registered, max_brightness=%d, helper=%s\n",
		 MAX_LEVEL, HELPER);
	return 0;
}

static void w737_bl_remove(struct platform_device *pdev)
{
	struct w737_bl *bl = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&bl->work);
}

static struct platform_driver w737_bl_driver = {
	.probe	= w737_bl_probe,
	.remove	= w737_bl_remove,
	.driver	= {
		.name = DRV_NAME,
	},
};
static struct platform_device *w737_bl_pdev;

static int __init w737_bl_init(void)
{
	int ret;

	ret = platform_driver_register(&w737_bl_driver);
	if (ret)
		return ret;

	/* Nothing in the device tree describes this shim, so create it here. */
	w737_bl_pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(w737_bl_pdev)) {
		platform_driver_unregister(&w737_bl_driver);
		return PTR_ERR(w737_bl_pdev);
	}
	return 0;
}

static void __exit w737_bl_exit(void)
{
	platform_device_unregister(w737_bl_pdev);
	platform_driver_unregister(&w737_bl_driver);
}

module_init(w737_bl_init);
module_exit(w737_bl_exit);

MODULE_DESCRIPTION("Galaxy Book2 AMOLED backlight shim");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);



