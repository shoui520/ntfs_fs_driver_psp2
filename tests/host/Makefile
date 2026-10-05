# Host test of the ntfsfs VFS glue + libntfs-3g against an NTFS image.
#   make VITASDK=/path/to/vitasdk [O=build] [CC=gcc] [EXTRA_CFLAGS=...]
# Only the SDK's psp2* headers are used; newlib headers are kept out of the
# include path by linking just those directories into $(O)/inc.

ifndef VITASDK
$(error set VITASDK to your VitaSDK path)
endif

ROOT := ../..
O ?= build
CC ?= gcc
SAN ?= -fsanitize=address,undefined

NTFS3G := $(wildcard $(ROOT)/third_party/ntfs-3g/libntfs-3g/*.c)
SRCS := test_ntfsfs.c mock_vita.c $(ROOT)/src/vfs.c $(ROOT)/src/devio.c $(ROOT)/src/acl.c $(NTFS3G)

CFLAGS := -O2 -g -std=gnu11 -fno-short-enums -DNTFSFS_HOST -DHAVE_CONFIG_H \
	-I. -I$(ROOT)/src -I$(ROOT)/src/port \
	-I$(ROOT)/third_party/ntfs-3g/include/ntfs-3g -I$(O)/inc \
	-Wall -Wno-unused -Wno-address-of-packed-member -Wno-zero-length-bounds \
	-Wno-maybe-uninitialized -Wno-use-after-free -Wno-pointer-to-int-cast \
	-Wno-int-to-pointer-cast -include linux/hdreg.h $(SAN) $(EXTRA_CFLAGS)

$(O)/test_ntfsfs: $(SRCS) | $(O)/inc
	$(CC) $(CFLAGS) -o $@ $(SRCS)

$(O)/inc:
	mkdir -p $@
	for d in psp2 psp2kern psp2common vitasdk; do \
		ln -sfn $(VITASDK)/arm-vita-eabi/include/$$d $@/$$d; done

clean:
	rm -rf $(O)

.PHONY: clean
