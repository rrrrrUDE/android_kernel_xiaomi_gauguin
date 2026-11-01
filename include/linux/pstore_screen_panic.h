/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PSTORE_SCREEN_PANIC_H
#define _LINUX_PSTORE_SCREEN_PANIC_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_PSTORE_SCREEN_PANIC)
int pstore_screen_panic_start(void);
void pstore_screen_panic_wait(void);
#else
static inline int pstore_screen_panic_start(void)
{
	return -EOPNOTSUPP;
}

static inline void pstore_screen_panic_wait(void)
{
}
#endif

#endif
