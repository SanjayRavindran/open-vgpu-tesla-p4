CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -fPIC -std=c11
LDFLAGS ?= -ldl

LIB_TARGET  := libopen-nvidia-encode.so
SONAME_LINK := libnvidia-encode.so.1
UNIT_BIN    := test_nvenc_unit

.PHONY: all test diff-test clean

all: $(LIB_TARGET) $(UNIT_BIN)

$(LIB_TARGET): open_nvenc.c open_nvenc.h
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(SONAME_LINK) -o $@ open_nvenc.c $(LDFLAGS)
	ln -sf $(LIB_TARGET) $(SONAME_LINK)

$(UNIT_BIN): test_nvenc_unit.c $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ test_nvenc_unit.c ./$(LIB_TARGET) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

test: $(UNIT_BIN)
	./$(UNIT_BIN)

diff-test: $(UNIT_BIN)
	OPEN_NVENC_VENDOR_LIB=/home/zero/vgpu-work/grid-guest-535.309.01/libnvidia-encode.so.535.309.01 ./$(UNIT_BIN)

clean:
	rm -f $(LIB_TARGET) $(SONAME_LINK) $(UNIT_BIN) *.o
