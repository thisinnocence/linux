// SPDX-License-Identifier: GPL-2.0-only
/*
 * Syslab sec XOR MMIO 字符设备驱动
 *
 * write 传入两个 U32 操作数并触发 XOR，read 返回 U32 结果，ioctl 清零结果
 */

#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <uapi/linux/sec.h>

#define SEC_DATA1	0x00
#define SEC_DATA2	0x04
#define SEC_CMD		0x08
#define SEC_RESULT	0x0c

struct sec_device {
	void __iomem *base;
	struct miscdevice miscdev;
	/* 保护一次完整的 register 事务 */
	struct mutex lock;
};

static struct sec_device *file_to_sec(struct file *file)
{
	return container_of(file->private_data, struct sec_device, miscdev);
}

static int sec_open(struct inode *inode, struct file *file)
{
	file->private_data = file_to_sec(file);
	return 0;
}

static int sec_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t sec_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	struct sec_device *sec = file->private_data;
	u32 result;

	if (count < sizeof(result))
		return -EINVAL;

	mutex_lock(&sec->lock);
	result = readl(sec->base + SEC_RESULT);
	mutex_unlock(&sec->lock);

	if (copy_to_user(buf, &result, sizeof(result)))
		return -EFAULT;

	return sizeof(result);
}

static ssize_t sec_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct sec_device *sec = file->private_data;
	struct sec_operands operands;

	if (count != sizeof(operands))
		return -EINVAL;
	if (copy_from_user(&operands, buf, sizeof(operands)))
		return -EFAULT;

	/* 保证两个操作数和执行命令组成一次完整事务 */
	mutex_lock(&sec->lock);
	writel(operands.data1, sec->base + SEC_DATA1);
	writel(operands.data2, sec->base + SEC_DATA2);
	writel(1, sec->base + SEC_CMD);
	mutex_unlock(&sec->lock);

	return sizeof(operands);
}

static long sec_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sec_device *sec = file->private_data;

	if (cmd != SEC_IOC_CLEAR)
		return -ENOTTY;

	mutex_lock(&sec->lock);
	writel(0, sec->base + SEC_CMD);
	mutex_unlock(&sec->lock);

	return 0;
}

static const struct file_operations sec_fops = {
	.owner = THIS_MODULE,
	.open = sec_open,
	.release = sec_release,
	.read = sec_read,
	.write = sec_write,
	.unlocked_ioctl = sec_ioctl,
	.llseek = no_llseek,
};

static int sec_probe(struct platform_device *pdev)
{
	struct sec_device *sec;
	int ret;

	sec = devm_kzalloc(&pdev->dev, sizeof(*sec), GFP_KERNEL);
	if (!sec)
		return -ENOMEM;

	sec->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sec->base))
		return PTR_ERR(sec->base);

	mutex_init(&sec->lock);
	sec->miscdev = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "sec",
		.fops = &sec_fops,
		.parent = &pdev->dev,
		.mode = 0600,
	};

	ret = misc_register(&sec->miscdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register misc device\n");

	platform_set_drvdata(pdev, sec);
	dev_info(&pdev->dev, "sec XOR character device ready\n");

	return 0;
}

static void sec_remove(struct platform_device *pdev)
{
	struct sec_device *sec = platform_get_drvdata(pdev);

	misc_deregister(&sec->miscdev);
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
	},
	.probe = sec_probe,
	.remove_new = sec_remove,
};
module_platform_driver(sec_driver);

MODULE_DESCRIPTION("Syslab sec XOR MMIO character device driver");
MODULE_LICENSE("GPL");
