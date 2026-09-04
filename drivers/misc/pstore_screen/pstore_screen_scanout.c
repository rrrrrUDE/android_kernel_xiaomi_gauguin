// SPDX-License-Identifier: GPL-2.0-only
#ifndef PSTORE_SCREEN_HOST
#include <linux/io.h>
#endif

#include "pstore_screen_core.h"

#define PS_SCANOUT_PATTERN_BYTES 64U

static int ps_scanout_address_validate(const struct ps_scanout *scanout)
{
	u64 last;
	unsigned long base;

	if (scanout->map_kind == PS_SCANOUT_MAP_WRITE_SPAN)
		return 0;
	if (!scanout->length)
		return -EOVERFLOW;
	last = scanout->length - 1U;
	if (last > (u64)~0UL)
		return -EOVERFLOW;
	if (scanout->map_kind == PS_SCANOUT_MAP_SYSTEM)
		base = (unsigned long)scanout->address.system;
	else
#ifdef PSTORE_SCREEN_HOST
		base = (unsigned long)scanout->address.io;
#else
		base = (__force unsigned long)scanout->address.io;
#endif
	if (base > ~0UL - (unsigned long)last)
		return -EOVERFLOW;
	return 0;
}

static u32 ps_format_cpp(u32 format)
{
	switch (format) {
	case PS_FORMAT_RGB565:
	case PS_FORMAT_BGR565:
		return 2U;
	case PS_FORMAT_XRGB8888:
	case PS_FORMAT_XBGR8888:
	case PS_FORMAT_ARGB8888:
	case PS_FORMAT_ABGR8888:
		return 4U;
	default:
		return 0U;
	}
}

int ps_pixel_pack(u32 format, u32 color, u8 out[4], u32 *out_len)
{
	u32 red;
	u32 green;
	u32 blue;
	u16 packed;

	if (!out || !out_len)
		return -EINVAL;
	red = (color >> 16) & 0xffU;
	green = (color >> 8) & 0xffU;
	blue = color & 0xffU;
	switch (format) {
	case PS_FORMAT_XRGB8888:
	case PS_FORMAT_ARGB8888:
		out[0] = (u8)blue;
		out[1] = (u8)green;
		out[2] = (u8)red;
		out[3] = format == PS_FORMAT_ARGB8888 ? 0xffU : 0U;
		*out_len = 4U;
		return 0;
	case PS_FORMAT_XBGR8888:
	case PS_FORMAT_ABGR8888:
		out[0] = (u8)red;
		out[1] = (u8)green;
		out[2] = (u8)blue;
		out[3] = format == PS_FORMAT_ABGR8888 ? 0xffU : 0U;
		*out_len = 4U;
		return 0;
	case PS_FORMAT_RGB565:
		packed = (u16)(((red >> 3) << 11) | ((green >> 2) << 5) |
			       (blue >> 3));
		break;
	case PS_FORMAT_BGR565:
		packed = (u16)(((blue >> 3) << 11) | ((green >> 2) << 5) |
			       (red >> 3));
		break;
	default:
		return -EINVAL;
	}
	out[0] = (u8)(packed & 0xffU);
	out[1] = (u8)(packed >> 8);
	out[2] = 0U;
	out[3] = 0U;
	*out_len = 2U;
	return 0;
}

int ps_scanout_validate(const struct ps_scanout *scanout)
{
	u32 cpp;
	u64 row_bytes;
	u64 required;

	if (!scanout || !scanout->width || !scanout->height ||
	    (scanout->flags & ~PS_SCANOUT_FLAGS_ALLOWED_MASK))
		return -EINVAL;
	cpp = ps_format_cpp(scanout->format);
	if (!cpp)
		return -EINVAL;
	row_bytes = (u64)scanout->width * cpp;
	if (row_bytes > scanout->pitch)
		return -EINVAL;
	required = (u64)(scanout->height - 1U) * scanout->pitch + row_bytes;
	if (required > scanout->length)
		return -EOVERFLOW;
	switch (scanout->map_kind) {
	case PS_SCANOUT_MAP_SYSTEM:
		if (!scanout->address.system ||
		    (!(scanout->flags & PS_SCANOUT_FLAG_CPU_COHERENT) &&
		     !scanout->flush))
			return -EINVAL;
		break;
	case PS_SCANOUT_MAP_IOMEM:
		if (!scanout->address.io)
			return -EINVAL;
		break;
	case PS_SCANOUT_MAP_WRITE_SPAN:
		if (!scanout->write_span ||
		    (!(scanout->flags & PS_SCANOUT_FLAG_WRITE_THROUGH) &&
		     !scanout->flush))
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	return ps_scanout_address_validate(scanout);
}

static int ps_scanout_write(struct ps_scanout *scanout, u64 offset,
			    const u8 *source, u32 length)
{
	if (!source || offset > scanout->length ||
	    length > scanout->length - offset)
		return -EOVERFLOW;
	if (!length)
		return 0;
	switch (scanout->map_kind) {
	case PS_SCANOUT_MAP_SYSTEM:
		memcpy((u8 *)scanout->address.system + (size_t)offset, source,
		       length);
		return 0;
	case PS_SCANOUT_MAP_IOMEM:
#ifdef PSTORE_SCREEN_HOST
		memcpy((u8 *)scanout->address.io + (size_t)offset, source, length);
#else
		memcpy_toio((u8 __iomem *)scanout->address.io + offset, source,
			    length);
#endif
		return 0;
	case PS_SCANOUT_MAP_WRITE_SPAN:
		return scanout->write_span(scanout->ctx, offset, source, length);
	default:
		return -EINVAL;
	}
}

static void ps_repeat_pixel(u8 *buffer, u32 length, const u8 *pixel, u32 cpp)
{
	u32 offset;

	for (offset = 0U; offset < length; offset += cpp)
		memcpy(buffer + offset, pixel, cpp);
}

int ps_scanout_fill_rect(struct ps_scanout *scanout, u32 x, u32 y,
			 u32 width, u32 height, u32 color)
{
	u8 pattern[PS_SCANOUT_PATTERN_BYTES];
	u8 pixel[4];
	u32 pattern_len;
	u32 cpp;
	u32 row;
	int ret;

	ret = ps_scanout_validate(scanout);
	if (ret)
		return ret;
	if (!width || !height || x >= scanout->width || y >= scanout->height ||
	    width > scanout->width - x || height > scanout->height - y)
		return -EINVAL;
	ret = ps_pixel_pack(scanout->format, color, pixel, &cpp);
	if (ret)
		return ret;
	pattern_len = (PS_SCANOUT_PATTERN_BYTES / cpp) * cpp;
	ps_repeat_pixel(pattern, pattern_len, pixel, cpp);
	for (row = 0U; row < height; row++) {
		u64 offset = (u64)(y + row) * scanout->pitch + (u64)x * cpp;
		u32 remaining = width * cpp;

		while (remaining) {
			u32 chunk = remaining < pattern_len ? remaining : pattern_len;

			ret = ps_scanout_write(scanout, offset, pattern, chunk);
			if (ret)
				return ret;
			offset += chunk;
			remaining -= chunk;
		}
	}
	return 0;
}

int ps_scanout_blit_mono(struct ps_scanout *scanout, u32 x, u32 y,
			 const u8 *data, u32 width, u32 height, u32 pitch,
			 u32 scale, u32 color)
{
	u64 bitmap_bytes;
	u32 scaled_width;
	u32 scaled_height;
	u32 row;
	int ret;

	ret = ps_scanout_validate(scanout);
	if (ret)
		return ret;
	bitmap_bytes = (u64)height * pitch;
	if (!data || !width || !height || !pitch || !scale ||
	    (u64)width > (u64)pitch * 8U ||
	    bitmap_bytes > (u64)(size_t)-1 ||
	    width > 0xffffffffU / scale ||
	    height > 0xffffffffU / scale)
		return -EINVAL;
	scaled_width = width * scale;
	scaled_height = height * scale;
	if (x >= scanout->width || y >= scanout->height ||
	    scaled_width > scanout->width - x ||
	    scaled_height > scanout->height - y)
		return -EINVAL;
	for (row = 0U; row < height; row++) {
		u32 column = 0U;

		while (column < width) {
			u32 start;
			size_t byte_offset;

			byte_offset = (size_t)row * pitch;

			while (column < width &&
			       !(data[byte_offset + column / 8U] &
				 (u8)(0x80U >> (column & 7U))))
				column++;
			start = column;
			while (column < width &&
			       (data[byte_offset + column / 8U] &
				(u8)(0x80U >> (column & 7U))))
				column++;
			if (start < column) {
				ret = ps_scanout_fill_rect(scanout, x + start * scale,
					y + row * scale, (column - start) * scale,
					scale, color);
				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

int ps_scanout_flush(struct ps_scanout *scanout)
{
	int ret;

	ret = ps_scanout_validate(scanout);
	if (ret)
		return ret;
#ifndef PSTORE_SCREEN_HOST
	/* Publish every pixel store before the provider-specific flush. */
	wmb();
#endif
	if (scanout->flush)
		return scanout->flush(scanout->ctx);
	return 0;
}

static int ps_scanout_fill_rect_op(void *ctx, u32 x, u32 y, u32 width,
				   u32 height, u32 color)
{
	return ps_scanout_fill_rect(ctx, x, y, width, height, color);
}

static int ps_scanout_blit_mono_op(void *ctx, u32 x, u32 y, const u8 *data,
				   u32 width, u32 height, u32 pitch, u32 scale,
				   u32 color)
{
	return ps_scanout_blit_mono(ctx, x, y, data, width, height, pitch, scale,
				    color);
}

int ps_scanout_draw_ops(struct ps_scanout *scanout, struct ps_draw_ops *ops)
{
	int ret;

	if (!ops)
		return -EINVAL;
	ret = ps_scanout_validate(scanout);
	if (ret)
		return ret;
	ops->ctx = scanout;
	ops->fill_rect = ps_scanout_fill_rect_op;
	ops->blit_mono = ps_scanout_blit_mono_op;
	return 0;
}
