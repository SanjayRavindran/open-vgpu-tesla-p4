CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -fPIC -std=c11
LDFLAGS ?= -ldl
PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib

LIB_TARGET  := libopen-nvidia-encode.so
SONAME_LINK := libnvidia-encode.so.1
UNIT_BIN    := test_nvenc_unit
HW_BIN      := test_nvenc_hw
DIFF_HW_BIN := test_nvenc_diff_hw
SRCS        := open_nvenc.c open_nvenc_hw.c open_nvenc_hevc_hdr.c open_nvenc_h264_hdr.c
HDRS        := open_nvenc.h open_nvenc_hw.h open_nvenc_hevc_hdr.h open_nvenc_h264_hdr.h

.PHONY: all test diff-test hw-test diff-hw-test install uninstall clean

all: $(LIB_TARGET) $(UNIT_BIN) $(HW_BIN) $(DIFF_HW_BIN)

$(LIB_TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(SONAME_LINK) -o $@ $(SRCS) $(LDFLAGS)
	ln -sf $(LIB_TARGET) $(SONAME_LINK)

$(UNIT_BIN): test_nvenc_unit.c $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ test_nvenc_unit.c ./$(LIB_TARGET) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

$(HW_BIN): test_nvenc_hw.c $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ test_nvenc_hw.c ./$(LIB_TARGET) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

$(DIFF_HW_BIN): test_nvenc_diff_hw.c $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ test_nvenc_diff_hw.c $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

test: $(UNIT_BIN)
	./$(UNIT_BIN)

diff-test: $(UNIT_BIN)
	OPEN_NVENC_VENDOR_LIB=/home/zero/vgpu-work/grid-guest-535.309.01/libnvidia-encode.so.535.309.01 ./$(UNIT_BIN)

hw-test: $(HW_BIN)
	./$(HW_BIN)

diff-hw-test: $(DIFF_HW_BIN)
	./$(DIFF_HW_BIN)

install: $(LIB_TARGET)
	install -d "$(DESTDIR)$(LIBDIR)"
	install -m 0755 $(LIB_TARGET) "$(DESTDIR)$(LIBDIR)/$(LIB_TARGET)"
	ln -sf $(LIB_TARGET) "$(DESTDIR)$(LIBDIR)/$(SONAME_LINK)"
	ln -sf $(SONAME_LINK) "$(DESTDIR)$(LIBDIR)/libnvidia-encode.so"

uninstall:
	rm -f "$(DESTDIR)$(LIBDIR)/$(LIB_TARGET)" "$(DESTDIR)$(LIBDIR)/$(SONAME_LINK)" "$(DESTDIR)$(LIBDIR)/libnvidia-encode.so"

clean:
	rm -f $(LIB_TARGET) $(SONAME_LINK) $(UNIT_BIN) $(HW_BIN) $(DIFF_HW_BIN) *.o
