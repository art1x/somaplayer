APOSTROPHE     := apostrophe
INCLUDE_DIR    := $(APOSTROPHE)/include
AP_RES_DIR     := $(APOSTROPHE)/res
SRC_DIR        := src
BUILD_DIR      := build
TG5040_TOOLCHAIN := ghcr.io/loveretro/tg5040-toolchain:latest
DOCKER          ?= $(shell command -v docker 2>/dev/null || echo "flatpak-spawn --host docker")

WARN_CFLAGS := -Wall -Wextra -Wno-unused-parameter
SRCS        := $(shell find $(SRC_DIR) -name '*.c')

CURL ?= $(shell pkg-config --exists libcurl 2>/dev/null && echo 1 || echo 0)
ifeq ($(CURL),1)
CURL_CFLAGS  := $(shell pkg-config --cflags libcurl) -DAP_ENABLE_CURL
CURL_LDFLAGS := $(shell pkg-config --libs libcurl)
else
CURL_CFLAGS  :=
CURL_LDFLAGS :=
endif

TMP_PAK   := $(BUILD_DIR)/tg5040/somaplayer.pak
FINAL_PAK := $(BUILD_DIR)/tg5040/SomaFM Radio.pak

.PHONY: linux tg5040 clean

linux:
	@mkdir -p $(BUILD_DIR)/linux
	cc -std=gnu11 -O0 -g $(WARN_CFLAGS) \
		-DPLATFORM_LINUX \
		-I$(INCLUDE_DIR) -I$(SRC_DIR) \
		$(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image) \
		$(CURL_CFLAGS) \
		-o $(BUILD_DIR)/linux/somaplayer \
		$(SRCS) \
		$(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image) \
		$(CURL_LDFLAGS) \
		-lm -lpthread
	@echo "→ $(BUILD_DIR)/linux/somaplayer"

tg5040:
	@mkdir -p '$(TMP_PAK)'
	$(DOCKER) run --rm \
		-v "$(CURDIR)":/workspace \
		-v "$(shell realpath $(APOSTROPHE))":/workspace/apostrophe \
		$(TG5040_TOOLCHAIN) \
		make -C /workspace -f ports/tg5040/Makefile \
			BUILD_DIR=/workspace/$(TMP_PAK)
	@cp -f launch.sh '$(TMP_PAK)/launch.sh'
	@chmod +x '$(TMP_PAK)/launch.sh'
	@CACERT=$$(find $(APOSTROPHE)/build/third_party/sources -name "cacert-*.pem" 2>/dev/null | sort | tail -1); \
	if [ -n "$$CACERT" ]; then cp -f "$$CACERT" "$(TMP_PAK)/cacert.pem"; fi
	@rm -rf '$(FINAL_PAK)'
	@mv '$(TMP_PAK)' '$(FINAL_PAK)'
	@echo "→ $(FINAL_PAK)/"

clean:
	rm -rf $(BUILD_DIR)
