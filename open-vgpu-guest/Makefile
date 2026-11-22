CC ?= gcc
CFLAGS := -O2 -Wall -Wextra -Werror -std=gnu11 -D_GNU_SOURCE -I. -I../open-vgpu-plugin -I../open-vgpu-mgr
PLUGIN_CORE_SRCS := \
	../open-vgpu-plugin/gp104_vgpu_core.c \
	../open-vgpu-plugin/open_vgpu_rm.c \
	../open-vgpu-plugin/open_vrpc_rm.c \
	../open-vgpu-plugin/open_vrpc_mmu.c \
	../open-vgpu-plugin/open_vgpu_static_info.c

HDRS := \
	open_vgpu_guest_rpc.h \
	../open-vgpu-plugin/open_vgpu_plugin.h \
	../open-vgpu-plugin/open_vgpu_rm.h \
	../open-vgpu-mgr/vmiop_env.h \
	../open-vgpu-mgr/nv_vgpu_vfio_abi.h

.PHONY: all test clean

all: test_host_guest_loopback

test_host_guest_loopback: test_host_guest_loopback.c open_vgpu_guest_rpc.c $(PLUGIN_CORE_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -rdynamic test_host_guest_loopback.c open_vgpu_guest_rpc.c $(PLUGIN_CORE_SRCS) -o $@ -lpthread -lrt

test: all
	./test_host_guest_loopback

clean:
	rm -f test_host_guest_loopback *.o
