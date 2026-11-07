// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/* Read BTF type names for resolve_btfids without loading BTF into the host kernel. */
#include "btf_local.h"

#include <errno.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct btf {
	unsigned char *raw;
	size_t size;
	const struct btf_type **types;
	unsigned int nr_types;
	const char *strings;
	size_t strings_len;
};

static int read_btf(const char *path, struct btf *btf)
{
	unsigned char magic[4];
	Elf *elf;
	Elf_Scn *scn = NULL;
	size_t names;
	int fd, ret = -1;
	ssize_t n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, magic, sizeof(magic));
	if (n != sizeof(magic)) {
		errno = EINVAL;
		goto out_fd;
	}
	if (lseek(fd, 0, SEEK_SET) < 0)
		goto out_fd;

	if (memcmp(magic, ELFMAG, SELFMAG)) {
		off_t size = lseek(fd, 0, SEEK_END);
		size_t done = 0;

		if (size <= 0 || lseek(fd, 0, SEEK_SET) < 0)
			goto out_fd;
		btf->raw = malloc(size);
		if (!btf->raw)
			goto out_fd;
		btf->size = size;
		while (done < btf->size) {
			n = read(fd, btf->raw + done, btf->size - done);
			if (n <= 0) {
				if (!n)
					errno = EINVAL;
				goto out_fd;
			}
			done += n;
		}
		ret = 0;
		goto out_fd;
	}

	if (elf_version(EV_CURRENT) == EV_NONE) {
		errno = EINVAL;
		goto out_fd;
	}
	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (!elf) {
		errno = EINVAL;
		goto out_fd;
	}
	if (elf_getshdrstrndx(elf, &names)) {
		errno = EINVAL;
		goto out_elf;
	}
	while ((scn = elf_nextscn(elf, scn))) {
		GElf_Shdr sh;
		Elf_Data *data;
		const char *name;

		if (!gelf_getshdr(scn, &sh))
			break;
		name = elf_strptr(elf, names, sh.sh_name);
		if (!name || strcmp(name, ".BTF"))
			continue;
		data = elf_getdata(scn, NULL);
		if (!data || !data->d_buf || !data->d_size)
			break;
		btf->raw = malloc(data->d_size);
		if (!btf->raw)
			goto out_elf;
		memcpy(btf->raw, data->d_buf, data->d_size);
		btf->size = data->d_size;
		ret = 0;
		break;
	}
	if (ret)
		errno = EINVAL;
out_elf:
	elf_end(elf);
out_fd:
	close(fd);
	return ret;
}

static size_t record_extra_size(unsigned int kind, unsigned int vlen)
{
	switch (kind) {
	case BTF_KIND_INT:
	case BTF_KIND_VAR:
	case 17: /* DECL_TAG */
		return 4;
	case BTF_KIND_ARRAY:
		return sizeof(struct btf_array);
	case BTF_KIND_STRUCT:
	case BTF_KIND_UNION:
		return vlen * sizeof(struct btf_member);
	case BTF_KIND_ENUM:
		return vlen * sizeof(struct btf_enum);
	case BTF_KIND_FUNC_PROTO:
		return vlen * sizeof(struct btf_param);
	case BTF_KIND_DATASEC:
		return vlen * sizeof(struct btf_var_secinfo);
	case 19: /* ENUM64 */
		return vlen * 12;
	case BTF_KIND_PTR:
	case BTF_KIND_FWD:
	case BTF_KIND_TYPEDEF:
	case BTF_KIND_VOLATILE:
	case BTF_KIND_CONST:
	case BTF_KIND_RESTRICT:
	case BTF_KIND_FUNC:
	case 16: /* FLOAT */
	case 18: /* TYPE_TAG */
		return 0;
	default:
		return SIZE_MAX;
	}
}

struct btf *btf__parse(const char *path)
{
	struct btf_header hdr;
	struct btf *btf = calloc(1, sizeof(*btf));
	size_t base, pos, end;
	unsigned int capacity = 0;

	if (!btf)
		return NULL;
	if (read_btf(path, btf))
		goto fail;
	if (btf->size < sizeof(hdr))
		goto invalid;
	memcpy(&hdr, btf->raw, sizeof(hdr));
	if (hdr.magic != BTF_MAGIC || hdr.version != BTF_VERSION ||
	    hdr.hdr_len < sizeof(hdr) || hdr.hdr_len > btf->size)
		goto invalid;
	base = hdr.hdr_len;
	if (hdr.type_off > btf->size - base ||
	    hdr.type_len > btf->size - base - hdr.type_off ||
	    hdr.str_off > btf->size - base ||
	    hdr.str_len > btf->size - base - hdr.str_off)
		goto invalid;
	btf->strings = (char *)btf->raw + base + hdr.str_off;
	btf->strings_len = hdr.str_len;
	pos = base + hdr.type_off;
	end = pos + hdr.type_len;
	while (pos < end) {
		struct btf_type type;
		const struct btf_type **types;
		size_t extra;

		if (end - pos < sizeof(type))
			goto invalid;
		memcpy(&type, btf->raw + pos, sizeof(type));
		extra = record_extra_size(BTF_LOCAL_KIND(type.info),
					  BTF_INFO_VLEN(type.info));
		if (extra == SIZE_MAX || extra > end - pos - sizeof(type) ||
		    btf->nr_types >= BTF_MAX_TYPE)
			goto invalid;
		if (btf->nr_types + 1 >= capacity) {
			capacity = capacity ? capacity * 2 : 256;
			types = realloc(btf->types, capacity * sizeof(*types));
			if (!types)
				goto fail;
			btf->types = types;
		}
		btf->types[++btf->nr_types] =
			(const struct btf_type *)(btf->raw + pos);
		pos += sizeof(type) + extra;
	}
	return btf;
invalid:
	errno = EINVAL;
fail:
	btf__free(btf);
	return NULL;
}

void btf__free(struct btf *btf)
{
	if (!btf)
		return;
	free(btf->types);
	free(btf->raw);
	free(btf);
}

unsigned int btf__get_nr_types(const struct btf *btf)
{
	return btf->nr_types;
}

const struct btf_type *btf__type_by_id(const struct btf *btf, unsigned int id)
{
	return id && id <= btf->nr_types ? btf->types[id] : NULL;
}

const char *btf__name_by_offset(const struct btf *btf, unsigned int off)
{
	const char *name;

	if (off >= btf->strings_len)
		return NULL;
	name = btf->strings + off;
	return memchr(name, '\0', btf->strings_len - off) ? name : NULL;
}
