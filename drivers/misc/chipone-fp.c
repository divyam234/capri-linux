// SPDX-License-Identifier: GPL-2.0-only
/* Chipone ICNF7332 power and interrupt control for secure-world clients. */
#include <linux/chipone-fp.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

struct chipone_fp {
	struct device *dev;
	struct miscdevice misc;
	struct gpio_desc *reset;
	struct regulator *vdd;
	struct kref ref;
	struct mutex lock; /* Power, open and removal state */
	struct mutex read_lock; /* Serialize readers */
	spinlock_t event_lock; /* Latest interrupt and pending flag */
	wait_queue_head_t wait;
	struct chipone_fp_event event;
	const char *firmware_name;
	int id;
	int irq;
	bool opened;
	bool powered;
	bool pending;
	bool removed;
};

static DEFINE_IDA(chipone_fp_ids);

static void chipone_fp_free(struct kref *ref)
{
	struct chipone_fp *fp = container_of(ref, struct chipone_fp, ref);

	ida_free(&chipone_fp_ids, fp->id);
	kfree(fp->misc.name);
	put_device(fp->dev);
	kfree(fp);
}

static int chipone_fp_power_on(struct chipone_fp *fp)
{
	int ret;

	if (!fp->powered) {
		ret = regulator_enable(fp->vdd);
		if (ret)
			return ret;
		fp->powered = true;
	}
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(fp->reset, 0);
	usleep_range(100, 150);
	gpiod_set_value_cansleep(fp->reset, 1);
	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(fp->reset, 0);
	usleep_range(1250, 1500);
	return 0;
}

static void chipone_fp_power_off(struct chipone_fp *fp)
{
	int ret;

	gpiod_set_value_cansleep(fp->reset, 1);
	if (!fp->powered)
		return;
	ret = regulator_disable(fp->vdd);
	if (ret)
		dev_err(fp->dev, "Failed to disable supply: %d\n", ret);
	else
		fp->powered = false;
}

static irqreturn_t chipone_fp_irq(int irq, void *data)
{
	struct chipone_fp *fp = data;
	unsigned long flags;

	spin_lock_irqsave(&fp->event_lock, flags);
	fp->event.timestamp_ns = ktime_get_ns();
	fp->event.sequence++;
	WRITE_ONCE(fp->pending, true);
	spin_unlock_irqrestore(&fp->event_lock, flags);
	wake_up_interruptible(&fp->wait);
	return IRQ_HANDLED;
}

static int chipone_fp_open(struct inode *inode, struct file *file)
{
	struct chipone_fp *fp = container_of(file->private_data, struct chipone_fp, misc);
	unsigned long flags;
	int ret;

	ret = mutex_lock_interruptible(&fp->lock);
	if (ret)
		return ret;
	if (fp->removed) {
		ret = -ENODEV;
		goto out;
	}
	if (fp->opened) {
		ret = -EBUSY;
		goto out;
	}
	ret = chipone_fp_power_on(fp);
	if (ret)
		goto out;
	spin_lock_irqsave(&fp->event_lock, flags);
	WRITE_ONCE(fp->pending, false);
	spin_unlock_irqrestore(&fp->event_lock, flags);
	fp->opened = true;
	kref_get(&fp->ref);
	file->private_data = fp;
	enable_irq(fp->irq);
	ret = stream_open(inode, file);
out:
	mutex_unlock(&fp->lock);
	return ret;
}

static int chipone_fp_release(struct inode *inode, struct file *file)
{
	struct chipone_fp *fp = file->private_data;

	mutex_lock(&fp->lock);
	if (!fp->removed) {
		disable_irq(fp->irq);
		chipone_fp_power_off(fp);
	}
	fp->opened = false;
	mutex_unlock(&fp->lock);
	kref_put(&fp->ref, chipone_fp_free);
	return 0;
}

static ssize_t chipone_fp_read(struct file *file, char __user *buffer,
			       size_t count, loff_t *offset)
{
	struct chipone_fp *fp = file->private_data;
	struct chipone_fp_event event;
	unsigned long flags;
	int ret;

	if (!count)
		return 0;
	if (count < sizeof(event))
		return -EINVAL;
	ret = mutex_lock_interruptible(&fp->read_lock);
	if (ret)
		return ret;
	if (!READ_ONCE(fp->pending) && !READ_ONCE(fp->removed)) {
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		ret = wait_event_interruptible(fp->wait,
					       READ_ONCE(fp->pending) || READ_ONCE(fp->removed));
		if (ret)
			goto out;
	}
	if (READ_ONCE(fp->removed)) {
		ret = -ENODEV;
		goto out;
	}
	spin_lock_irqsave(&fp->event_lock, flags);
	event = fp->event;
	spin_unlock_irqrestore(&fp->event_lock, flags);
	if (copy_to_user(buffer, &event, sizeof(event))) {
		ret = -EFAULT;
		goto out;
	}
	spin_lock_irqsave(&fp->event_lock, flags);
	if (fp->event.sequence == event.sequence)
		WRITE_ONCE(fp->pending, false);
	spin_unlock_irqrestore(&fp->event_lock, flags);
	ret = sizeof(event);
out:
	mutex_unlock(&fp->read_lock);
	return ret;
}

static __poll_t chipone_fp_poll(struct file *file, poll_table *wait)
{
	struct chipone_fp *fp = file->private_data;

	poll_wait(file, &fp->wait, wait);
	if (READ_ONCE(fp->removed))
		return EPOLLERR | EPOLLHUP;
	if (READ_ONCE(fp->pending))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations chipone_fp_fops = {
	.owner = THIS_MODULE,
	.open = chipone_fp_open,
	.release = chipone_fp_release,
	.read = chipone_fp_read,
	.poll = chipone_fp_poll,
};

static ssize_t firmware_name_show(struct device *dev,
				  struct device_attribute *attr, char *buffer)
{
	struct miscdevice *misc = dev_get_drvdata(dev);
	struct chipone_fp *fp = container_of(misc, struct chipone_fp, misc);

	return sysfs_emit(buffer, "%s\n", fp->firmware_name);
}
static DEVICE_ATTR_RO(firmware_name);

static struct attribute *chipone_fp_attrs[] = {
	&dev_attr_firmware_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(chipone_fp);

static int chipone_fp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct chipone_fp *fp;
	int ret;

	fp = kzalloc_obj(*fp);
	if (!fp)
		return -ENOMEM;
	fp->id = ida_alloc(&chipone_fp_ids, GFP_KERNEL);
	if (fp->id < 0) {
		ret = fp->id;
		kfree(fp);
		return ret;
	}
	fp->dev = get_device(dev);
	kref_init(&fp->ref);
	mutex_init(&fp->lock);
	mutex_init(&fp->read_lock);
	spin_lock_init(&fp->event_lock);
	init_waitqueue_head(&fp->wait);
	fp->misc.name = kasprintf(GFP_KERNEL, "chipone-fp%d", fp->id);
	if (!fp->misc.name) {
		ret = -ENOMEM;
		goto put;
	}
	ret = device_property_read_string(dev, "firmware-name", &fp->firmware_name);
	if (ret)
		goto put;
	fp->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(fp->reset)) {
		ret = PTR_ERR(fp->reset);
		goto put;
	}
	fp->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(fp->vdd)) {
		ret = PTR_ERR(fp->vdd);
		goto put;
	}
	fp->irq = platform_get_irq(pdev, 0);
	if (fp->irq < 0) {
		ret = fp->irq;
		goto put;
	}
	ret = devm_request_irq(dev, fp->irq, chipone_fp_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), fp);
	if (ret)
		goto put;
	fp->misc.minor = MISC_DYNAMIC_MINOR;
	fp->misc.fops = &chipone_fp_fops;
	fp->misc.parent = dev;
	fp->misc.groups = chipone_fp_groups;
	fp->misc.mode = 0600;
	ret = misc_register(&fp->misc);
	if (ret) {
		devm_free_irq(dev, fp->irq, fp);
		goto put;
	}
	platform_set_drvdata(pdev, fp);
	return 0;
put:
	kref_put(&fp->ref, chipone_fp_free);
	return dev_err_probe(dev, ret, "Failed to register fingerprint control\n");
}

static void chipone_fp_remove(struct platform_device *pdev)
{
	struct chipone_fp *fp = platform_get_drvdata(pdev);

	misc_deregister(&fp->misc);
	mutex_lock(&fp->lock);
	WRITE_ONCE(fp->removed, true);
	if (fp->opened)
		disable_irq(fp->irq);
	chipone_fp_power_off(fp);
	mutex_unlock(&fp->lock);
	devm_free_irq(&pdev->dev, fp->irq, fp);
	wake_up_interruptible(&fp->wait);
	kref_put(&fp->ref, chipone_fp_free);
}

static const struct of_device_id chipone_fp_of_match[] = {
	{ .compatible = "chipone,icnf7332" },
	{ }
};
MODULE_DEVICE_TABLE(of, chipone_fp_of_match);

static struct platform_driver chipone_fp_driver = {
	.probe = chipone_fp_probe,
	.remove = chipone_fp_remove,
	.driver = {
		.name = "chipone-fp",
		.of_match_table = chipone_fp_of_match,
	},
};
module_platform_driver(chipone_fp_driver);

MODULE_DESCRIPTION("Chipone ICNF7332 secure-world sensor control");
MODULE_LICENSE("GPL");
