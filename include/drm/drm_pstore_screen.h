/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DRM_PSTORE_SCREEN_H
#define _DRM_PSTORE_SCREEN_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/pstore_screen.h>

struct drm_device;
struct drm_framebuffer;
struct drm_gem_object;

#define PSTORE_SCREEN_DRM_SIDECAR_DESC_V1_VERSION 1U
#define PSTORE_SCREEN_DRM_SIDECAR_DESC_VERSION \
	PSTORE_SCREEN_DRM_SIDECAR_DESC_V1_VERSION

#define PSTORE_SCREEN_DRM_SIDECAR_ACTIVE_SCANOUT 0x00000001U
#define PSTORE_SCREEN_DRM_SIDECAR_MAPPING_PREPARED 0x00000002U
#define PSTORE_SCREEN_DRM_SIDECAR_RESOURCES_PINNED 0x00000004U
#define PSTORE_SCREEN_DRM_SIDECAR_REQUIRED_FLAGS 0x00000006U
#define PSTORE_SCREEN_DRM_SIDECAR_ALLOWED_FLAGS 0x00000007U

/*
 * v1 layout is KMI-frozen; append fields only through a new descriptor type
 * and symbol. A larger caller-owned allocation is accepted, but its trailing
 * bytes are never interpreted by the v1 entry points.
 */
struct pstore_screen_drm_sidecar_desc {
	u32 size;
	u32 version;
	const char *name;
	struct module *owner;
	struct drm_framebuffer *framebuffer;
	struct drm_gem_object *gem;
	u32 priority;
	u32 flags;
	u64 capabilities;
	u32 width_mm;
	u32 height_mm;
	u32 map_kind;
	u32 map_flags;
	u64 map_length;
	union pstore_screen_scanout_address address;
	void *ctx;
	int (*write_span)(void *ctx, u64 offset, const void *source, u32 length);
	int (*flush)(void *ctx);
	struct pstore_screen_display_ops ops;
};

#define PSTORE_SCREEN_DRM_SIDECAR_DESC_V1_SIZE \
	(offsetof(struct pstore_screen_drm_sidecar_desc, ops) + \
	 offsetof(struct pstore_screen_display_ops, release) + \
	 sizeof(((struct pstore_screen_display_ops *)0)->release))

struct pstore_screen_drm_sidecar;

#if IS_REACHABLE(CONFIG_PSTORE_SCREEN_DRM_SIDECAR)
int pstore_screen_drm_sidecar_register(
		const struct pstore_screen_drm_sidecar_desc *desc,
		struct pstore_screen_drm_sidecar **sidecar);
void pstore_screen_drm_sidecar_unregister(
		struct pstore_screen_drm_sidecar *sidecar);
#else
static inline int pstore_screen_drm_sidecar_register(
		const struct pstore_screen_drm_sidecar_desc *desc,
		struct pstore_screen_drm_sidecar **sidecar)
{
	return -EOPNOTSUPP;
}

static inline void pstore_screen_drm_sidecar_unregister(
		struct pstore_screen_drm_sidecar *sidecar)
{
}
#endif

#endif
