// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"

u32 ps_crc32_begin(void)
{
	return 0xffffffffU;
}

u32 ps_crc32_feed(u32 state, const u8 *data, u32 len)
{
	u32 i;

	if (!data && len)
		return state;

	for (i = 0; i < len; i++) {
		u32 byte = data[i];
		u32 bit;

		state ^= byte;
		for (bit = 0; bit < 8U; bit++)
			state = (state >> 1) ^
				 ((state & 1U) ? 0xedb88320U : 0U);
	}

	return state;
}

u32 ps_crc32_end(u32 state)
{
	return state ^ 0xffffffffU;
}

u32 ps_crc32(const u8 *data, u32 len)
{
	return ps_crc32_end(ps_crc32_feed(ps_crc32_begin(), data, len));
}

struct ps_sha256_state {
	u32 h[8];
};

static u32 ps_rotr32(u32 value, u32 shift)
{
	return (value >> shift) | (value << (32U - shift));
}

static u32 ps_get_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
	       ((u32)p[2] << 8) | (u32)p[3];
}

static void ps_put_be32(u8 *p, u32 value)
{
	p[0] = (u8)(value >> 24);
	p[1] = (u8)(value >> 16);
	p[2] = (u8)(value >> 8);
	p[3] = (u8)value;
}

static void ps_sha256_transform(struct ps_sha256_state *state,
				const u8 block[64])
{
	static const u32 k[64] = {
		0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
		0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
		0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
		0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
		0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
		0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
		0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
		0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
		0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
		0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
		0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
		0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
		0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
		0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
		0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
		0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
	};
	u32 w[64];
	u32 a, b, c, d, e, f, g, h;
	u32 i;

	for (i = 0; i < 16U; i++)
		w[i] = ps_get_be32(block + i * 4U);
	for (i = 16U; i < 64U; i++) {
		u32 s0 = ps_rotr32(w[i - 15U], 7U) ^
			 ps_rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
		u32 s1 = ps_rotr32(w[i - 2U], 17U) ^
			 ps_rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);

		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}

	a = state->h[0];
	b = state->h[1];
	c = state->h[2];
	d = state->h[3];
	e = state->h[4];
	f = state->h[5];
	g = state->h[6];
	h = state->h[7];

	for (i = 0; i < 64U; i++) {
		u32 s1 = ps_rotr32(e, 6U) ^ ps_rotr32(e, 11U) ^
			 ps_rotr32(e, 25U);
		u32 choose = (e & f) ^ ((~e) & g);
		u32 temp1 = h + s1 + choose + k[i] + w[i];
		u32 s0 = ps_rotr32(a, 2U) ^ ps_rotr32(a, 13U) ^
			 ps_rotr32(a, 22U);
		u32 majority = (a & b) ^ (a & c) ^ (b & c);
		u32 temp2 = s0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	state->h[0] += a;
	state->h[1] += b;
	state->h[2] += c;
	state->h[3] += d;
	state->h[4] += e;
	state->h[5] += f;
	state->h[6] += g;
	state->h[7] += h;
}

void ps_sha256(const u8 *data, u32 len, u8 out[32])
{
	struct ps_sha256_state state = {
		.h = {
			0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
			0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
			0x1f83d9abU, 0x5be0cd19U,
		},
	};
	u8 block[64];
	u32 remaining = len;
	u32 offset = 0;
	u32 tail;
	u64 bit_len = (u64)len * 8U;
	u32 i;

	if (!out)
		return;
	if (!data && len) {
		memset(out, 0, 32U);
		return;
	}

	while (remaining >= 64U) {
		ps_sha256_transform(&state, data + offset);
		offset += 64U;
		remaining -= 64U;
	}

	memset(block, 0, sizeof(block));
	if (remaining)
		memcpy(block, data + offset, remaining);
	block[remaining] = 0x80U;
	tail = remaining;
	if (tail >= 56U) {
		ps_sha256_transform(&state, block);
		memset(block, 0, sizeof(block));
	}
	for (i = 0; i < 8U; i++)
		block[63U - i] = (u8)(bit_len >> (i * 8U));
	ps_sha256_transform(&state, block);

	for (i = 0; i < 8U; i++)
		ps_put_be32(out + i * 4U, state.h[i]);
	memset(&state, 0, sizeof(state));
	memset(block, 0, sizeof(block));
}
