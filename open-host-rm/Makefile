CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -fPIC -std=c11

LIB_TARGET := libopen-host-rm-gp104.so
UNIT_BIN   := test_open_host_rm_unit

.PHONY: all test clean

all: $(LIB_TARGET) $(UNIT_BIN)

$(LIB_TARGET): open_host_rm_gp104.c open_host_rm_gp104.h
	$(CC) $(CFLAGS) -shared -o $@ open_host_rm_gp104.c

$(UNIT_BIN): test_open_host_rm_unit.c $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ test_open_host_rm_unit.c ./$(LIB_TARGET) -Wl,-rpath,'$$ORIGIN'

test: $(UNIT_BIN)
	./$(UNIT_BIN)

clean:
	rm -f $(LIB_TARGET) $(UNIT_BIN) *.o
