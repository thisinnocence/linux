/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SEC_H
#define _UAPI_LINUX_SEC_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* write 使用的两个 U32 操作数 */
struct sec_operands {
	__u32 data1;
	__u32 data2;
};

#define SEC_DMA_MAX_LEN	64

/* SEC_IOC_DMA_COPY 使用的固定大小 DMA copy transaction */
struct sec_dma_copy {
	__u32 len;
	__u8 src[SEC_DMA_MAX_LEN];
	__u8 dst[SEC_DMA_MAX_LEN];
};

#define SEC_IOC_MAGIC	's'
#define SEC_IOC_CLEAR	_IO(SEC_IOC_MAGIC, 0)
#define SEC_IOC_GET_IRQ_COUNT	_IOR(SEC_IOC_MAGIC, 1, __u32)
#define SEC_IOC_DMA_COPY	_IOWR(SEC_IOC_MAGIC, 2, struct sec_dma_copy)

#endif /* _UAPI_LINUX_SEC_H */
