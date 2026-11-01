/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PSTORE_SCREEN_GAUGUIN_H
#define _LINUX_PSTORE_SCREEN_GAUGUIN_H

#include <linux/types.h>

int qpnp_pon_panic_poweroff_type(u8 type);

#define PSTORE_SCREEN_KEY_POWER 1U
#define PSTORE_SCREEN_KEY_VOLUME_UP 2U
#define PSTORE_SCREEN_KEY_VOLUME_DOWN 3U
#define PSTORE_SCREEN_KEY_PRESS 1U
#define PSTORE_SCREEN_KEY_RELEASE 2U

#endif
