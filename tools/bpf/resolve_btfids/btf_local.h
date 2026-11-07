/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
#ifndef RESOLVE_BTFIDS_BTF_LOCAL_H
#define RESOLVE_BTFIDS_BTF_LOCAL_H

#include "../../../include/uapi/linux/btf.h"
#include <stddef.h>

struct btf;

/* Recent pahole may emit kinds above 15; this kernel UAPI masks to 4 bits. */
#define BTF_LOCAL_KIND(info) (((info) >> 24) & 0x1f)

struct btf *btf__parse(const char *path);
void btf__free(struct btf *btf);
unsigned int btf__get_nr_types(const struct btf *btf);
const struct btf_type *btf__type_by_id(const struct btf *btf, unsigned int id);
const char *btf__name_by_offset(const struct btf *btf, unsigned int off);

static inline int btf_is_func(const struct btf_type *t)
{
	return BTF_LOCAL_KIND(t->info) == BTF_KIND_FUNC;
}

static inline int btf_is_struct(const struct btf_type *t)
{
	return BTF_LOCAL_KIND(t->info) == BTF_KIND_STRUCT;
}

static inline int btf_is_union(const struct btf_type *t)
{
	return BTF_LOCAL_KIND(t->info) == BTF_KIND_UNION;
}

static inline int btf_is_typedef(const struct btf_type *t)
{
	return BTF_LOCAL_KIND(t->info) == BTF_KIND_TYPEDEF;
}

#endif
