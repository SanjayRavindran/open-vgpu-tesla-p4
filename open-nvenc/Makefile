CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -fPIC -std=c11
LDFLAGS ?= -ldl

LIB_TARGET  := libopen-nvidia-encode.so
SONAME_LINK := libnvidia-encode.so.1
UNIT_BIN    := test_nvenc_unit
HW_BIN      := test_nvenc_hw
SRCS        := open_nvenc.c open_nvenc_hw.c open_nvenc_hevc_hdr.c open_nvenc_h264_hdr.c
HDRS        := open_nvenc.h open_nvenc_hw.h open_nvenc_hevc_hdr.h open_nvenc_h264_hdr.h

.PHONY: all test diff-test hw-test clean

all: $(LIB_TARGET) $(UNIT_BIN) $(HW_BIN)

$(LIB_TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(SONAME_LINK) -o $@ $(SRCS) $(LDFLAGS)
	ln -sf $(LIB_TARGET) $(SONAME_LINK)

$(UNIT_BIN): test_nvenc_unit.c $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ test_nvenc_unit.c ./$(LIB_TARGET) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

$(HW_BIN): test_nvenc_hw.c $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ test_nvenc_hw.c ./$(LIB_TARGET) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

test: $(UNIT_BIN)
	./$(UNIT_BIN)

diff-test: $(UNIT_BIN)
	OPEN_NVENC_VENDOR_LIB=/home/zero/vgpu-work/grid-guest-535.309.01/libnvidia-encode.so.535.309.01 ./$(UNIT_BIN)

hw-test: $(HW_BIN)
	./$(HW_BIN)

clean:
	rm -f $(LIB_TARGET) $(SONAME_LINK) $(UNIT_BIN) $(HW_BIN) *.o
