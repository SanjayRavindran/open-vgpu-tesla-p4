CC ?= gcc
CFLAGS := -O2 -Wall -Wextra -Werror

BIN := open-nvidia-xid-logd
MOCK_SO := mock_nvml.so

all: $(BIN)

$(BIN): open_xid_logd.c
	$(CC) $(CFLAGS) $< -ldl -o $@

$(MOCK_SO): mock_nvml.c
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@

test: $(BIN) $(MOCK_SO)
	bash ./test_xid_logd.sh

clean:
	rm -f $(BIN) $(MOCK_SO)

.PHONY: all test clean
