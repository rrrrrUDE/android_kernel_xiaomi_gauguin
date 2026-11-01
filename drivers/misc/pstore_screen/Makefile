# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += -I$(src)/../../../include

obj-$(CONFIG_PSTORE_SCREEN) += pstore_screen.o

pstore_screen-y := \
	pstore_screen_meta.o \
	pstore_screen_hash.o \
	pstore_screen_pslc.o \
	pstore_screen_text.o \
	pstore_screen_font.o \
	pstore_screen_layout.o \
	pstore_screen_qr.o \
	pstore_screen_render.o \
	qrcodegen.o

pstore_screen-$(CONFIG_PSTORE_SCREEN_LOG_CAPTURE) += pstore_screen_log.o
pstore_screen-$(CONFIG_PSTORE_SCREEN_PANIC) += \
	pstore_screen_input.o \
	pstore_screen_panic.o

pstore_screen-$(CONFIG_PSTORE_SCREEN_DISPLAY) += \
	pstore_screen_scanout.o \
	pstore_screen_display_core.o \
	pstore_screen_display.o

obj-$(CONFIG_PSTORE_SCREEN_DRM_SIDECAR) += pstore_screen_drm_sidecar.o

pstore_screen_drm_sidecar-y := pstore_screen_drm.o

# The fixed upstream QR implementation uses C99 loop declarations. Keep the
# language-mode exception local to this vendored translation unit.
CFLAGS_qrcodegen.o += -std=gnu11 -Wno-declaration-after-statement
