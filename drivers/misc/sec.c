// SPDX-License-Identifier: GPL-2.0-only
/*
 * Syslab sec XOR 和 DMA MMIO 字符设备驱动
 *
 * write/read 验证 PIO XOR，ioctl 使用 coherent buffer 验证 SMMUv3 DMA
 */

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/interrupt.h>
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
#define SEC_IRQ_STATUS	0x10
#define SEC_DMA_SRC_LO	0x14
#define SEC_DMA_SRC_HI	0x18
#define SEC_DMA_DST_LO	0x1c
#define SEC_DMA_DST_HI	0x20
#define SEC_DMA_LEN	0x24
#define SEC_DMA_CMD	0x28
#define SEC_DMA_STATUS	0x2c

#define SEC_IRQ_PENDING	BIT(0)
#define SEC_DMA_DONE	BIT(0)
#define SEC_IRQ_TIMEOUT_MS	1000

struct sec_device {
	void __iomem *base;
	struct miscdevice miscdev;
	atomic_t irq_count;
	struct completion command_done;
	void *dma_src;
	dma_addr_t dma_src_addr;
	void *dma_dst;
	dma_addr_t dma_dst_addr;
	u32 dma_status;
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
	long ret;

	if (count != sizeof(operands))
		return -EINVAL;
	if (copy_from_user(&operands, buf, sizeof(operands)))
		return -EFAULT;

	/* 保证两个操作数和执行命令组成一次完整事务 */
	mutex_lock(&sec->lock);
	reinit_completion(&sec->command_done);
	writel(operands.data1, sec->base + SEC_DATA1);
	writel(operands.data2, sec->base + SEC_DATA2);
	writel(1, sec->base + SEC_CMD);
	ret = wait_for_completion_timeout(
		&sec->command_done, msecs_to_jiffies(SEC_IRQ_TIMEOUT_MS));
	mutex_unlock(&sec->lock);
	if (!ret)
		return -ETIMEDOUT;

	return sizeof(operands);
}

static long sec_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sec_device *sec = file->private_data;
	struct sec_dma_copy transaction;
	u32 dma_status;
	u32 irq_count;
	long ret;

	if (cmd == SEC_IOC_GET_IRQ_COUNT) {
		irq_count = atomic_read(&sec->irq_count);
		if (copy_to_user((void __user *)arg, &irq_count,
				 sizeof(irq_count)))
			return -EFAULT;
		return 0;
	}

	if (cmd == SEC_IOC_DMA_COPY) {
		if (copy_from_user(&transaction, (void __user *)arg,
				   sizeof(transaction)))
			return -EFAULT;
		if (!transaction.len || transaction.len > SEC_DMA_MAX_LEN)
			return -EINVAL;

		mutex_lock(&sec->lock);
		memcpy(sec->dma_src, transaction.src, transaction.len);
		memset(sec->dma_dst, 0, transaction.len);
		sec->dma_status = 0;
		reinit_completion(&sec->command_done);
		dma_wmb();
		writel(lower_32_bits(sec->dma_src_addr),
		       sec->base + SEC_DMA_SRC_LO);
		writel(upper_32_bits(sec->dma_src_addr),
		       sec->base + SEC_DMA_SRC_HI);
		writel(lower_32_bits(sec->dma_dst_addr),
		       sec->base + SEC_DMA_DST_LO);
		writel(upper_32_bits(sec->dma_dst_addr),
		       sec->base + SEC_DMA_DST_HI);
		writel(transaction.len, sec->base + SEC_DMA_LEN);
		writel(1, sec->base + SEC_DMA_CMD);
		ret = wait_for_completion_timeout(
			&sec->command_done,
			msecs_to_jiffies(SEC_IRQ_TIMEOUT_MS));
		if (ret)
			dma_rmb();
		dma_status = sec->dma_status;
		if (ret && dma_status == SEC_DMA_DONE)
			memcpy(transaction.dst, sec->dma_dst,
			       transaction.len);
		mutex_unlock(&sec->lock);

		if (!ret)
			return -ETIMEDOUT;
		if (dma_status != SEC_DMA_DONE)
			return -EIO;
		if (copy_to_user((void __user *)arg, &transaction,
				 sizeof(transaction)))
			return -EFAULT;
		return 0;
	}

	if (cmd != SEC_IOC_CLEAR)
		return -ENOTTY;

	mutex_lock(&sec->lock);
	writel(0, sec->base + SEC_CMD);
	mutex_unlock(&sec->lock);

	return 0;
}

static irqreturn_t sec_irq_handler(int irq, void *data)
{
	struct sec_device *sec = data;
	u32 dma_status;
	u32 result;

	if (!(readl(sec->base + SEC_IRQ_STATUS) & SEC_IRQ_PENDING))
		return IRQ_NONE;

	dma_status = readl(sec->base + SEC_DMA_STATUS);
	result = readl(sec->base + SEC_RESULT);
	if (dma_status)
		writel(dma_status, sec->base + SEC_DMA_STATUS);
	writel(SEC_IRQ_PENDING, sec->base + SEC_IRQ_STATUS);
	sec->dma_status = dma_status;
	atomic_inc(&sec->irq_count);
	if (dma_status)
		pr_info("[sec-irq]: dma status=0x%08x\n", dma_status);
	else
		pr_info("[sec-irq]: result=0x%08x\n", result);
	complete(&sec->command_done);

	return IRQ_HANDLED;
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
	int irq;
	int ret;

	sec = devm_kzalloc(&pdev->dev, sizeof(*sec), GFP_KERNEL);
	if (!sec)
		return -ENOMEM;

	sec->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sec->base))
		return PTR_ERR(sec->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to set DMA mask\n");

	sec->dma_src = dmam_alloc_coherent(&pdev->dev, SEC_DMA_MAX_LEN,
					   &sec->dma_src_addr, GFP_KERNEL);
	if (!sec->dma_src)
		return -ENOMEM;
	sec->dma_dst = dmam_alloc_coherent(&pdev->dev, SEC_DMA_MAX_LEN,
					   &sec->dma_dst_addr, GFP_KERNEL);
	if (!sec->dma_dst)
		return -ENOMEM;

	atomic_set(&sec->irq_count, 0);
	init_completion(&sec->command_done);
	ret = devm_request_irq(&pdev->dev, irq, sec_irq_handler, 0,
			       dev_name(&pdev->dev), sec);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request interrupt\n");

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
	dev_info(&pdev->dev,
		 "sec XOR and DMA character device ready, src=%pad dst=%pad\n",
		 &sec->dma_src_addr, &sec->dma_dst_addr);

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

MODULE_DESCRIPTION("Syslab sec XOR and DMA MMIO character device driver");
MODULE_LICENSE("GPL");
