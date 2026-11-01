/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PSTORE_SCREEN_H
#define _LINUX_PSTORE_SCREEN_H

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kconfig.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/types.h>

#define PSTORE_SCREEN_DISPLAY_NAME_LEN 32U
#define PSTORE_SCREEN_DISPLAY_MAX_PROVIDERS 8U

#define PSTORE_SCREEN_MAP_SYSTEM 1U
#define PSTORE_SCREEN_MAP_IOMEM 2U
#define PSTORE_SCREEN_MAP_WRITE_SPAN 3U

#define PSTORE_SCREEN_SCANOUT_CPU_COHERENT 0x00000001U
#define PSTORE_SCREEN_SCANOUT_WRITE_THROUGH 0x00000002U

#define PSTORE_SCREEN_PROVIDER_ACTIVE_SCANOUT 0x00000001U
#define PSTORE_SCREEN_PROVIDER_ALLOWED_FLAGS 0x00000001U

#define PSTORE_SCREEN_PROVIDER_CAP_RESERVE_NO_SIDE_EFFECT (1ULL << 0)
#define PSTORE_SCREEN_PROVIDER_CAP_CHECK_NO_SIDE_EFFECT (1ULL << 1)
#define PSTORE_SCREEN_PROVIDER_CAP_ENTER_PANIC_SAFE (1ULL << 2)
#define PSTORE_SCREEN_PROVIDER_CAP_WRITE_SPAN_PANIC_SAFE (1ULL << 3)
#define PSTORE_SCREEN_PROVIDER_CAP_FLUSH_PANIC_SAFE (1ULL << 4)
#define PSTORE_SCREEN_PROVIDER_CAP_RELEASE_PANIC_SAFE (1ULL << 5)
#define PSTORE_SCREEN_PROVIDER_CAPABILITIES_MASK ((1ULL << 6) - 1ULL)

#define PSTORE_SCREEN_STAGE_FIRMWARE 1U
#define PSTORE_SCREEN_STAGE_DRM 2U
#define PSTORE_SCREEN_STAGE_CONTROLLER 3U

#define PSTORE_SCREEN_PRIORITY_FIRMWARE 100U
#define PSTORE_SCREEN_PRIORITY_DRM 200U
#define PSTORE_SCREEN_PRIORITY_CONTROLLER 300U

#define PSTORE_SCREEN_DISPLAY_DESC_V1_VERSION 1U
#define PSTORE_SCREEN_DISPLAY_DESC_VERSION PSTORE_SCREEN_DISPLAY_DESC_V1_VERSION

union pstore_screen_scanout_address {
	void *system;
	void __iomem *io;
};

struct pstore_screen_scanout_desc {
	u32 map_kind;
	u32 format;
	u32 flags;
	u32 width;
	u32 height;
	u32 pitch;
	u32 width_mm;
	u32 height_mm;
	u64 length;
	union pstore_screen_scanout_address address;
	void *ctx;
	int (*write_span)(void *ctx, u64 offset, const void *source, u32 length);
	int (*flush)(void *ctx);
};

struct pstore_screen_display_ops {
	int (*reserve)(void *ctx, u64 generation, u64 *token);
	int (*check)(void *ctx, u64 generation, u64 token);
	int (*enter)(void *ctx, u64 generation, u64 token);
	void (*release)(void *ctx, u64 generation, u64 token);
};

/*
 * v1 layout is KMI-frozen; append fields only through a new descriptor type
 * and symbol. A larger caller-owned allocation is accepted, but its trailing
 * bytes are never interpreted by the v1 entry points.
 */
struct pstore_screen_display_desc {
	u32 size;
	u32 version;
	const char *name;
	struct module *owner;
	u32 stage;
	u32 priority;
	u32 flags;
	u64 capabilities;
	struct pstore_screen_scanout_desc scanout;
	void *ctx;
	struct pstore_screen_display_ops ops;
};

#define PSTORE_SCREEN_DISPLAY_DESC_V1_SIZE \
	(offsetof(struct pstore_screen_display_desc, ops) + \
	 offsetof(struct pstore_screen_display_ops, release) + \
	 sizeof(((struct pstore_screen_display_ops *)0)->release))

struct pstore_screen_display_handle;

#if IS_ENABLED(CONFIG_PSTORE_SCREEN_DISPLAY)
int pstore_screen_display_register(
		const struct pstore_screen_display_desc *desc,
		struct pstore_screen_display_handle **handle);
void pstore_screen_display_unregister(
		struct pstore_screen_display_handle *handle);
#else
static inline int pstore_screen_display_register(
		const struct pstore_screen_display_desc *desc,
		struct pstore_screen_display_handle **handle)
{
	return -EOPNOTSUPP;
}

static inline void pstore_screen_display_unregister(
		struct pstore_screen_display_handle *handle)
{
}
#endif

#endif
