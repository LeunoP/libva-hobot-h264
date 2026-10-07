CC ?= gcc
CFLAGS ?= -O2 -Wall -fPIC -pthread -I/usr/include
LDFLAGS ?= -shared -L/usr/hobot/lib -lva -lva-drm -lmultimedia -lhbmem -Wl,-rpath=/usr/hobot/lib
TEST_BUILD_DIR ?= /tmp/libva-hobot-tests
TEST_CFLAGS ?= -O2 -Wall -Wextra -pthread -I/usr/include -Iinclude
VA_TEST_LIBS ?= -L/usr/hobot/lib -Wl,-rpath=/usr/hobot/lib -lva -lva-drm
HOBOT_TEST_LIBS ?= -L/usr/hobot/lib -Wl,-rpath=/usr/hobot/lib -lmultimedia -lhbmem
FFMPEG_TEST_CFLAGS ?= $(shell pkg-config --cflags libavformat libavcodec libavutil)
FFMPEG_TEST_LIBS ?= $(shell pkg-config --libs libavformat libavcodec libavutil)

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

$(TEST_BUILD_DIR):
	mkdir -p $@

$(TEST_BUILD_DIR)/test_va_config: tools/test_va_config.c | $(TEST_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $< -o $@ $(VA_TEST_LIBS)

$(TEST_BUILD_DIR)/test_va_surface_state: tools/test_va_surface_state.c src/hobot_drv_video.c include/va/va_hobot.h | $(TEST_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -Wno-unused-parameter $< -o $@ $(VA_TEST_LIBS) $(HOBOT_TEST_LIBS)

$(TEST_BUILD_DIR)/test_va_surface_export: tools/test_va_surface_export.c include/va/va_hobot.h | $(TEST_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $(FFMPEG_TEST_CFLAGS) $< -o $@ $(FFMPEG_TEST_LIBS) $(VA_TEST_LIBS)

$(TEST_BUILD_DIR)/test_va_surface_import: tools/test_va_surface_import.c include/va/va_hobot.h | $(TEST_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $< -o $@ $(VA_TEST_LIBS)

$(TEST_BUILD_DIR)/test_va_jpeg_encode: tools/test_va_jpeg_encode.c | $(TEST_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $< -o $@ $(VA_TEST_LIBS)

$(TEST_BUILD_DIR)/test_va_jpeg_rotation: tools/test_va_jpeg_rotation.c | $(TEST_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $< -o $@ $(VA_TEST_LIBS)

$(TEST_BUILD_DIR)/test_hevc_wpp_encode: tools/test_hevc_wpp_encode.c | $(TEST_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $< -o $@ $(VA_TEST_LIBS)

build-va-tests: $(TEST_BUILD_DIR)/test_va_config $(TEST_BUILD_DIR)/test_va_surface_state $(TEST_BUILD_DIR)/test_va_surface_export $(TEST_BUILD_DIR)/test_va_surface_import $(TEST_BUILD_DIR)/test_va_jpeg_encode $(TEST_BUILD_DIR)/test_va_jpeg_rotation $(TEST_BUILD_DIR)/test_hevc_wpp_encode

test-va-state: $(TEST_BUILD_DIR)/test_va_surface_state
	$<

clean:
	rm -f $(TARGET) test_vpu_c test_sps_gen

.PHONY: all install clean build-va-tests test-va-state
