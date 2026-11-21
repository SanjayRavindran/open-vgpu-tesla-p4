CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11 -D_GNU_SOURCE

.PHONY: all clean test

all: libopen-nvidia-ml.so open-nvidia-smi test_nvml_smi_unit

libopen-nvidia-ml.so: open_nvml.c open_nvml.h
	$(CC) $(CFLAGS) -fPIC -shared -Wl,-soname,libnvidia-ml.so.1 open_nvml.c -lpthread -ldl -o $@

open-nvidia-smi: open_nvidia_smi.c open_nvml.h libopen-nvidia-ml.so
	ln -sf libopen-nvidia-ml.so libnvidia-ml.so.1
	$(CC) $(CFLAGS) open_nvidia_smi.c -L. -l:libopen-nvidia-ml.so -Wl,-rpath,'$$ORIGIN' -o $@

test_nvml_smi_unit: test_nvml_smi_unit.c open_nvml.h libopen-nvidia-ml.so open-nvidia-smi
	$(CC) $(CFLAGS) test_nvml_smi_unit.c -L. -l:libopen-nvidia-ml.so -ldl -Wl,-rpath,'$$ORIGIN' -o $@

test: all
	ln -sf libopen-nvidia-ml.so libnvidia-ml.so.1
	./test_nvml_smi_unit; rc=$$?; rm -f libnvidia-ml.so.1; exit $$rc

clean:
	rm -f libopen-nvidia-ml.so libnvidia-ml.so.1 open-nvidia-smi test_nvml_smi_unit *.o
