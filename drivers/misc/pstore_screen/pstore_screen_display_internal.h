/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PSTORE_SCREEN_DISPLAY_INTERNAL_H
#define _PSTORE_SCREEN_DISPLAY_INTERNAL_H

#include "pstore_screen_core.h"

struct pstore_screen_display_handle;

struct pstore_screen_kernel_claim {
	struct pstore_screen_display_handle *provider;
	struct ps_display_claim core;
	struct ps_draw_ops draw_ops;
};

int pstore_screen_display_preclaim_internal(
		struct pstore_screen_kernel_claim *claim);
int pstore_screen_display_begin_internal(
		struct pstore_screen_kernel_claim *claim);
int pstore_screen_display_present_internal(
		struct pstore_screen_kernel_claim *claim);
void pstore_screen_display_mark_lost_internal(
		struct pstore_screen_kernel_claim *claim);
int pstore_screen_display_abort_internal(
		struct pstore_screen_kernel_claim *claim);
const struct ps_layout *pstore_screen_display_layout_internal(
		const struct pstore_screen_kernel_claim *claim);

#endif
