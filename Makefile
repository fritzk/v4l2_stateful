SYSROOT=/build/trogdor

INCLUDE=/usr/include /usr/include/libdrm
CC=/usr/bin/armv7a-cros-linux-gnueabihf-clang
CFLAGS := -g -Wall $(foreach d, $(INCLUDE), -I$(SYSROOT)$(d))
all : decode encode

decode: v4l2_stateful_decoder.o dmabuf.o
	$(CC) $(CFLAGS) -o $@ $^ -L/build/trogdor/usr/lib -ldrm --sysroot=$(SYSROOT)

encode: v4l2_stateful_encoder.o
	$(CC) $(CFLAGS) -o $@ $^ -L/build/trogdor/usr/lib -ldrm --sysroot=$(SYSROOT)

clean:
	rm -f decode encode *.o
