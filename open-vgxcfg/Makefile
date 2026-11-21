CC ?= gcc
CFLAGS := -O2 -Wall -Wextra -Werror -fPIC

SO_TARGET := libopen-vgxcfg.so
UNIT_BIN := test_vgxcfg_unit
DIFF_BIN := test_vgxcfg_diff

all: $(SO_TARGET)

$(SO_TARGET): open_vgxcfg.c open_vgxcfg.h
	$(CC) $(CFLAGS) -shared -Wl,-soname,libnvidia-vgxcfg.so.1 $< -o $@

$(UNIT_BIN): test_vgxcfg_unit.c open_vgxcfg.h $(SO_TARGET)
	$(CC) -O2 -Wall -Wextra -Werror $< -L. -l:libopen-vgxcfg.so -Wl,-rpath,. -o $@

$(DIFF_BIN): test_vgxcfg_diff.c
	$(CC) -O2 -Wall -Wextra -Werror $< -ldl -o $@

test: $(UNIT_BIN)
	ln -sf $(SO_TARGET) libnvidia-vgxcfg.so.1
	./$(UNIT_BIN); rc=$$?; rm -f libnvidia-vgxcfg.so.1; exit $$rc

diff-test: $(SO_TARGET) $(DIFF_BIN)
	@test -n "$(ORIG_VGXCFG_SO)" || { echo "Set ORIG_VGXCFG_SO=/path/to/libnvidia-vgxcfg.so.535.309.01"; exit 2; }
	@python3 -c 'import tempfile, os, subprocess; xml = """<?xml version="1.0" encoding="utf-8"?>\n<vgxconfig>\n  <globalSettings>\n    <homogeneousUsms>FALSE</homogeneousUsms>\n    <licenseServer>192.168.1.24</licenseServer>\n  </globalSettings>\n  <pgpu>\n    <devId vendorId="0x10de" deviceId="0x1bb3" subsystemVendorId="0x10de" subsystemId="0x0"/>\n    <homogeneousUsms>TRUE</homogeneousUsms>\n    <supportedUsm usmId="65">\n      <devId vendorId="0x10de" deviceId="0x1bb3" subsystemVendorId="0x10de" subsystemId="0x1206"/>\n      <maxUsms>2</maxUsms>\n      <digest type="signature">deadbeef</digest>\n    </supportedUsm>\n  </pgpu>\n  <usmType id="65" name="GRID P4-4Q" class="Quadro">\n    <framebuffer>0x1000</framebuffer>\n    <license>QVDWS;VWS</license>\n    <display width="3840" height="2160"/>\n    <display width="2560" height="1440"/>\n  </usmType>\n</vgxconfig>\n"""; f = tempfile.NamedTemporaryFile("w", delete=False); f.write(xml); f.close(); rc = subprocess.call(["./$(DIFF_BIN)", "$(ORIG_VGXCFG_SO)", "./$(SO_TARGET)", f.name]); os.unlink(f.name); raise SystemExit(rc)'

clean:
	rm -f $(SO_TARGET) $(UNIT_BIN) $(DIFF_BIN)

.PHONY: all test diff-test clean
