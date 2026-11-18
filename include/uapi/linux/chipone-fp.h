/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_CHIPONE_FP_H
#define _UAPI_LINUX_CHIPONE_FP_H

#include <linux/types.h>

/* Draft interface: not an upstream Linux ABI. */
struct chipone_fp_event {
	__aligned_u64 timestamp_ns;
	__aligned_u64 sequence;
};

#endif
