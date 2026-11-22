CC ?= gcc
CFLAGS := -O2 -Wall -Wextra -Werror -std=gnu11 -D_GNU_SOURCE
SHLDFLAGS := -shared -ldl -lpthread -lrt

HDRS := open_vgpu_plugin.h open_vgpu_rm.h ../open-vgpu-mgr/vmiop_env.h ../open-vgpu-mgr/nv_vgpu_vfio_abi.h
CORE_SRCS := open_vgpu_plugin.c gp104_vgpu_core.c open_vgpu_rm.c open_vrpc_rm.c

.PHONY: all test clean

all: libopen-nvidia-vgpu.so mock_vendor_vgpu.so test_vgpu_plugin_unit

libopen-nvidia-vgpu.so: $(CORE_SRCS) vmiop_plugin.map $(HDRS)
	$(CC) $(CFLAGS) -fPIC -I../open-vgpu-mgr \
		-Wl,-soname,libnvidia-vgpu.so \
		-Wl,--version-script=vmiop_plugin.map \
		$(CORE_SRCS) -o $@ $(SHLDFLAGS)

mock_vendor_vgpu.so: mock_vendor_vgpu.c vmiop_plugin.map $(HDRS)
	$(CC) $(CFLAGS) -fPIC -I../open-vgpu-mgr \
		-Wl,-soname,libnvidia-vgpu.so \
		-Wl,--version-script=vmiop_plugin.map \
		mock_vendor_vgpu.c -o $@ $(SHLDFLAGS)

test_vgpu_plugin_unit: test_vgpu_plugin_unit.c gp104_vgpu_core.c open_vgpu_rm.c open_vrpc_rm.c $(HDRS) libopen-nvidia-vgpu.so mock_vendor_vgpu.so
	$(CC) $(CFLAGS) -I../open-vgpu-mgr -rdynamic \
		test_vgpu_plugin_unit.c gp104_vgpu_core.c open_vgpu_rm.c open_vrpc_rm.c -o $@ -ldl -lpthread -lrt

test: all
	./test_vgpu_plugin_unit

clean:
	rm -f libopen-nvidia-vgpu.so mock_vendor_vgpu.so test_vgpu_plugin_unit *.o
