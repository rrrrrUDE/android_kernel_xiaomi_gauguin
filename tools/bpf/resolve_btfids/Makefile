# SPDX-License-Identifier: GPL-2.0-only
# Host-side BTF ID resolver. OUTPUT is the kernel's object-tree directory.
srctree := $(abspath ../../..)
OUTPUT ?= $(CURDIR)

CC := $(if $(HOSTCC),$(HOSTCC),cc)
CFLAGS := $(KBUILD_HOSTCFLAGS) -O2 -Wall -D_GNU_SOURCE \
	-I$(srctree)/tools/include -I$(srctree)/tools/include/uapi
LDLIBS := -lelf

ifeq ($(V),1)
Q :=
else
Q := @
quiet = @printf '  %-8s %s\n' "$(1)" "$(2)"
endif

all: $(OUTPUT)/resolve_btfids

$(OUTPUT):
	$(call quiet,MKDIR,$@)
	$(Q)mkdir -p $@

$(OUTPUT)/main.o: main.c btf_local.h | $(OUTPUT)
	$(call quiet,HOSTCC,$(@F))
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(OUTPUT)/btf_local.o: btf_local.c btf_local.h | $(OUTPUT)
	$(call quiet,HOSTCC,$(@F))
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(OUTPUT)/rbtree.o: $(srctree)/tools/lib/rbtree.c | $(OUTPUT)
	$(call quiet,HOSTCC,$(@F))
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(OUTPUT)/resolve_btfids: $(OUTPUT)/main.o $(OUTPUT)/btf_local.o $(OUTPUT)/rbtree.o
	$(call quiet,HOSTLD,$(@F))
	$(Q)$(CC) $(KBUILD_HOSTLDFLAGS) -o $@ $^ $(LDLIBS) $(KBUILD_HOSTLDLIBS)

clean:
	$(Q)rm -f $(OUTPUT)/main.o $(OUTPUT)/btf_local.o $(OUTPUT)/rbtree.o \
		$(OUTPUT)/resolve_btfids

.PHONY: all clean
