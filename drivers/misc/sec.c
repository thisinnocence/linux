// SPDX-License-Identifier: GPL-2.0-only
/*
 * Syslab sec XOR MMIO 设备驱动
 *
 * 通过 sysfs 直接暴露四个 U32 register，便于在 mini-virt guest 中实验
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define SEC_DATA1	0x00
#define SEC_DATA2	0x04
#define SEC_CMD		0x08
#define SEC_RESULT	0x0c

struct sec_device {
	void __iomem *base;
};

static ssize_t data1_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct sec_device *sec = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", readl(sec->base + SEC_DATA1));
}

static ssize_t data1_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct sec_device *sec = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;

	writel(value, sec->base + SEC_DATA1);
	return count;
}
static DEVICE_ATTR_RW(data1);

static ssize_t data2_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct sec_device *sec = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", readl(sec->base + SEC_DATA2));
}

static ssize_t data2_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct sec_device *sec = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;

	writel(value, sec->base + SEC_DATA2);
	return count;
}
static DEVICE_ATTR_RW(data2);

static ssize_t cmd_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct sec_device *sec = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", readl(sec->base + SEC_CMD));
}

static ssize_t cmd_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct sec_device *sec = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;
	if (value > 1)
		return -EINVAL;

	writel(value, sec->base + SEC_CMD);
	return count;
}
static DEVICE_ATTR_RW(cmd);

static ssize_t result_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct sec_device *sec = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", readl(sec->base + SEC_RESULT));
}
static DEVICE_ATTR_RO(result);

static struct attribute *sec_attrs[] = {
	&dev_attr_data1.attr,
	&dev_attr_data2.attr,
	&dev_attr_cmd.attr,
	&dev_attr_result.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sec);

static int sec_probe(struct platform_device *pdev)
{
	struct sec_device *sec;

	sec = devm_kzalloc(&pdev->dev, sizeof(*sec), GFP_KERNEL);
	if (!sec)
		return -ENOMEM;

	sec->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sec->base))
		return PTR_ERR(sec->base);

	platform_set_drvdata(pdev, sec);
	dev_info(&pdev->dev, "sec XOR device ready\n");

	return 0;
}

static const struct of_device_id sec_of_match[] = {
	{ .compatible = "syslab,sec" },
	{ }
};
MODULE_DEVICE_TABLE(of, sec_of_match);

static struct platform_driver sec_driver = {
	.driver = {
		.name = "syslab-sec",
		.of_match_table = sec_of_match,
		.dev_groups = sec_groups,
	},
	.probe = sec_probe,
};
module_platform_driver(sec_driver);

MODULE_DESCRIPTION("Syslab sec XOR MMIO device driver");
MODULE_LICENSE("GPL");
