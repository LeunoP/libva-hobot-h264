CC ?= gcc
CFLAGS ?= -O2 -Wall -I/usr/include -I/usr/include/libdrm
LDFLAGS ?= -L/usr/hobot/lib -lEGL -lGLESv2 -lgbm -ldrm -lhbmem -Wl,-rpath=/usr/hobot/lib
BENCH_LDFLAGS ?= $(LDFLAGS) -lavcodec -lavformat -lavutil -lva -lva-drm -ldl

TARGETS = test_directviv test_render_directviv test_nv12_overlay test_va_directviv_bench test_va_config test_va_surface_state test_va_surface_export test_va_jpeg_encode

test_va_config: test_va_config.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -lva-drm -lva -o $@

test_va_surface_state: test_va_surface_state.c ../src/hobot_drv_video.c
	$(CC) $(CFLAGS) -pthread $< $(LDFLAGS) -lmultimedia -o $@

test_va_surface_export: test_va_surface_export.c
	$(CC) $(CFLAGS) $< $(BENCH_LDFLAGS) -o $@

test_va_jpeg_encode: test_va_jpeg_encode.c
	$(CC) $(CFLAGS) $< -L/usr/hobot/lib -lva-drm -lva -Wl,-rpath=/usr/hobot/lib -o $@

all: $(TARGETS)

test_directviv: test_directviv.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

test_render_directviv: test_render_directviv.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

test_nv12_overlay: test_nv12_overlay.c
	$(CC) $(CFLAGS) $< -ldrm -lhbmem -L/usr/hobot/lib -Wl,-rpath=/usr/hobot/lib -o $@

test_va_directviv_bench: test_va_directviv_bench.c
	$(CC) $(CFLAGS) $< $(BENCH_LDFLAGS) -o $@

clean:
	rm -f $(TARGETS)

.PHONY: all clean
