CC ?= gcc
CFLAGS := -O2 -Wall -Wextra -Werror -std=gnu11 -D_GNU_SOURCE
LDFLAGS := -Wl,--export-dynamic -lpthread -ldl -lrt

COMMON_SRCS := rm_client.c vmiop_env.c
DAEMON_SRCS := open_vgpu_mgr.c $(COMMON_SRCS)
TEST_SRCS   := test_vgpu_mgr_unit.c $(COMMON_SRCS)

HDRS := nv_vgpu_vfio_abi.h rm_client.h vmiop_env.h

.PHONY: all test clean

all: open-nvidia-vgpu-mgr

open-nvidia-vgpu-mgr: $(DAEMON_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRCS) $(LDFLAGS)

test_vgpu_mgr_unit: $(TEST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS) $(LDFLAGS)

test: test_vgpu_mgr_unit
	./test_vgpu_mgr_unit

clean:
	rm -f open-nvidia-vgpu-mgr test_vgpu_mgr_unit *.o
