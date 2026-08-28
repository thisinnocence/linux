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

#define SEC_IOC_MAGIC	's'
#define SEC_IOC_CLEAR	_IO(SEC_IOC_MAGIC, 0)

#endif /* _UAPI_LINUX_SEC_H */
