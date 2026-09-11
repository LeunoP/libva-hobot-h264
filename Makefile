CC ?= gcc
CFLAGS ?= -O2 -Wall -fPIC -I/usr/include
LDFLAGS ?= -shared -L/usr/hobot/lib -lva -lva-drm -lmultimedia -lhbmem -Wl,-rpath=/usr/hobot/lib

TARGET = hobot_drv_video.so
SRCS = src/hobot_drv_video.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

install: $(TARGET)
	install -d /usr/lib/aarch64-linux-gnu/dri
	install -m 755 $(TARGET) /usr/lib/aarch64-linux-gnu/dri/$(TARGET)
	install -d /usr/include/va
	install -m 644 include/va/va_hobot.h /usr/include/va/va_hobot.h

clean:
	rm -f $(TARGET) test_vpu_c test_sps_gen

.PHONY: all install clean
