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
#include <linux/iommu.h>
#include <linux/slab.h>
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
#define SEC_VF_ID	0x30
#define SEC_SID		0x34
#define SEC_RESET	0x38

#define SEC_IRQ_PENDING	BIT(0)
#define SEC_DMA_DONE	BIT(0)
#define SEC_DMA_ERROR	BIT(1)
#define SEC_IRQ_TIMEOUT_MS	1000

static bool fault_test;
module_param(fault_test, bool, 0400);
MODULE_PARM_DESC(fault_test, "Enable controlled unmapped-IOVA test ioctl");

struct sec_device {
	struct device *dev;
	struct sec_vf_info info;
	atomic_t opened;
	int irq;
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

/* 本模型的 DMA 在 MMIO callback 中同步完成，复位时没有后台请求 */
static void sec_reset_locked(struct sec_device *sec)
{
	writel(1, sec->base + SEC_RESET);
	synchronize_irq(sec->irq);
	sec->dma_status = 0;
	reinit_completion(&sec->command_done);
	memset(sec->dma_src, 0, SEC_DMA_MAX_LEN);
	memset(sec->dma_dst, 0, SEC_DMA_MAX_LEN);
}

static int sec_open(struct inode *inode, struct file *file)
{
	struct sec_device *sec = file_to_sec(file);

	if (atomic_cmpxchg(&sec->opened, 0, 1))
		return -EBUSY;
	file->private_data = sec;
	return 0;
}

static int sec_release(struct inode *inode, struct file *file)
{
	struct sec_device *sec = file->private_data;

	mutex_lock(&sec->lock);
	sec_reset_locked(sec);
	mutex_unlock(&sec->lock);
	atomic_set(&sec->opened, 0);
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
	if (!ret)
		sec_reset_locked(sec);
	mutex_unlock(&sec->lock);
	if (!ret)
		return -ETIMEDOUT;

	return sizeof(operands);
}

/* 调用方持有当前 VF 的 lock，返回前对应 IRQ handler 已执行 */
static int sec_submit_dma(struct sec_device *sec, dma_addr_t src, u32 len)
{
	long ret;

	sec->dma_status = 0;
	reinit_completion(&sec->command_done);
	dma_wmb();
	writel(lower_32_bits(src), sec->base + SEC_DMA_SRC_LO);
	writel(upper_32_bits(src), sec->base + SEC_DMA_SRC_HI);
	writel(lower_32_bits(sec->dma_dst_addr), sec->base + SEC_DMA_DST_LO);
	writel(upper_32_bits(sec->dma_dst_addr), sec->base + SEC_DMA_DST_HI);
	writel(len, sec->base + SEC_DMA_LEN);
	writel(1, sec->base + SEC_DMA_CMD);
	ret = wait_for_completion_timeout(&sec->command_done,
				msecs_to_jiffies(SEC_IRQ_TIMEOUT_MS));
	if (!ret) {
		sec_reset_locked(sec);
		return -ETIMEDOUT;
	}
	dma_rmb();
	return sec->dma_status == SEC_DMA_DONE ? 0 : -EIO;
}

/* 先访问有效 mapping 预热 IOTLB，再撤销 mapping 并验证旧 IOVA 被拒绝 */
static int sec_test_fault(struct sec_device *sec)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(sec->dev);
	dma_addr_t iova;
	void *buf;
	int ret;

	if (!fault_test)
		return -EOPNOTSUPP;
	if (!domain || domain->type != IOMMU_DOMAIN_DMA)
		return -EOPNOTSUPP;
	buf = kmalloc(SEC_DMA_MAX_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memset(buf, 0x5a, SEC_DMA_MAX_LEN);
	iova = dma_map_single(sec->dev, buf, SEC_DMA_MAX_LEN, DMA_TO_DEVICE);
	if (dma_mapping_error(sec->dev, iova)) {
		kfree(buf);
		return -EIO;
	}
	ret = sec_submit_dma(sec, iova, SEC_DMA_MAX_LEN);
	if (!ret && memchr_inv(sec->dma_dst, 0x5a, SEC_DMA_MAX_LEN))
		ret = -EIO;
	/* strict domain 在 unmap 返回前完成页表及 IOTLB invalidation */
	dma_unmap_single(sec->dev, iova, SEC_DMA_MAX_LEN, DMA_TO_DEVICE);
	if (ret)
		goto out;
	if (iommu_iova_to_phys(domain, iova)) {
		ret = -EIO;
		goto out;
	}
	memset(sec->dma_dst, 0xa5, SEC_DMA_MAX_LEN);
	ret = sec_submit_dma(sec, iova, sizeof(u32));
	if (ret == -EIO && sec->dma_status == SEC_DMA_ERROR &&
	    !memchr_inv(sec->dma_dst, 0xa5, SEC_DMA_MAX_LEN)) {
		dev_info(sec->dev, "VF%u SID%u unmapped IOVA %pad rejected\n",
			 sec->info.vf_id, sec->info.sid, &iova);
		ret = 0;
	} else if (!ret) {
		ret = -EIO;
	}
out:
	kfree(buf);
	return ret;
}

static long sec_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sec_device *sec = file->private_data;
	struct sec_dma_copy transaction;
	u32 irq_count;
	long ret;

	if (cmd == SEC_IOC_GET_INFO) {
		if (copy_to_user((void __user *)arg, &sec->info, sizeof(sec->info)))
			return -EFAULT;
		return 0;
	}
	if (cmd == SEC_IOC_RESET || cmd == SEC_IOC_TEST_FAULT) {
		mutex_lock(&sec->lock);
		if (cmd == SEC_IOC_RESET) {
			sec_reset_locked(sec);
			ret = 0;
		} else {
			ret = sec_test_fault(sec);
		}
		mutex_unlock(&sec->lock);
		return ret;
	}

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
		ret = sec_submit_dma(sec, sec->dma_src_addr, transaction.len);
		if (!ret)
			memcpy(transaction.dst, sec->dma_dst, transaction.len);
		mutex_unlock(&sec->lock);
		if (ret)
			return ret;
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
		dev_dbg(sec->dev, "dma status=0x%08x\n", dma_status);
	else
		dev_dbg(sec->dev, "result=0x%08x\n", result);
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
	struct iommu_fwspec *fwspec;
	struct iommu_domain *domain;
	phys_addr_t src_pa, dst_pa;
	int irq;
	int ret;

	sec = devm_kzalloc(&pdev->dev, sizeof(*sec), GFP_KERNEL);
	if (!sec)
		return -ENOMEM;

	sec->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sec->base))
		return PTR_ERR(sec->base);

	sec->dev = &pdev->dev;
	sec->info.vf_id = readl(sec->base + SEC_VF_ID);
	sec->info.sid = readl(sec->base + SEC_SID);
	sec->info.max_dma_len = SEC_DMA_MAX_LEN;
	sec->info.flags = fault_test ? SEC_VF_F_FAULT_TEST : 0;
	fwspec = dev_iommu_fwspec_get(&pdev->dev);
	domain = iommu_get_domain_for_dev(&pdev->dev);
	if (sec->info.vf_id >= 4 || !fwspec || fwspec->num_ids != 1 ||
	    fwspec->ids[0] != sec->info.sid || !domain ||
	    !iommu_is_dma_domain(domain))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "VF topology requires matching SID and DMA domain\n");

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

	sec->irq = irq;
	mutex_init(&sec->lock);
	atomic_set(&sec->opened, 0);
	atomic_set(&sec->irq_count, 0);
	writel(1, sec->base + SEC_RESET);
	init_completion(&sec->command_done);
	ret = devm_request_irq(&pdev->dev, irq, sec_irq_handler, 0,
			       dev_name(&pdev->dev), sec);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request interrupt\n");

	sec->miscdev = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR,
		.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "sec%u",
				       sec->info.vf_id),
		.fops = &sec_fops,
		.parent = &pdev->dev,
		.mode = 0600,
	};

	if (!sec->miscdev.name)
		return -ENOMEM;

	ret = misc_register(&sec->miscdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register misc device\n");

	platform_set_drvdata(pdev, sec);
	src_pa = iommu_iova_to_phys(domain, sec->dma_src_addr);
	dst_pa = iommu_iova_to_phys(domain, sec->dma_dst_addr);
	dev_info(&pdev->dev,
		 "VF%u SID%u ready: src IOVA=%pad PA=%pa dst IOVA=%pad PA=%pa\n",
		 sec->info.vf_id, sec->info.sid, &sec->dma_src_addr, &src_pa,
		 &sec->dma_dst_addr, &dst_pa);

	return 0;
}

static void sec_remove(struct platform_device *pdev)
{
	struct sec_device *sec = platform_get_drvdata(pdev);

	misc_deregister(&sec->miscdev);
}

static const struct of_device_id sec_of_match[] = {
	{ .compatible = "syslab,sec-vf" },
	{ }
};
MODULE_DEVICE_TABLE(of, sec_of_match);

static struct platform_driver sec_driver = {
	.driver = {
		.name = "syslab-sec",
		.of_match_table = sec_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = sec_probe,
	.remove_new = sec_remove,
};
module_platform_driver(sec_driver);

MODULE_DESCRIPTION("Syslab sec XOR and DMA MMIO character device driver");
MODULE_LICENSE("GPL");
