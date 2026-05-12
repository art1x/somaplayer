CROSS_COMPILE ?= aarch64-nextui-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
PLATFORM_ID := tg5040
THIRD_PARTY_SCRIPT := /workspace/apostrophe/scripts/build_third_party.sh

PKG_CONFIG  := pkg-config
SDL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LDFLAGS := $(shell $(PKG_CONFIG) --libs sdl2) -lSDL2_ttf -lSDL2_image

BUILD_DIR ?= /workspace/build/tg5040/somaplayer.pak
SRCS      := $(shell find /workspace/src -name '*.c')

CURL_PREFIX    := /workspace/apostrophe/build/third_party/$(PLATFORM_ID)/curl/install
OPENSSL_PREFIX := /workspace/apostrophe/build/third_party/$(PLATFORM_ID)/openssl/install
OPENSSL_LIBSSL    := $(OPENSSL_PREFIX)/lib/libssl.a
OPENSSL_LIBCRYPTO := $(OPENSSL_PREFIX)/lib/libcrypto.a
CURL_STAMP  := $(CURL_PREFIX)/.stamp-curl-8.7.1-static2

MPG123_SCRIPT  := /workspace/scripts/build_mpg123.sh
MPG123_PREFIX  := /workspace/build/third_party/$(PLATFORM_ID)/mpg123
MPG123_BIN     := $(MPG123_PREFIX)/bin/mpg123
MPG123_STAMP   := $(MPG123_PREFIX)/.stamp-mpg123-1.32.9

CURL_CFLAGS  := -I$(CURL_PREFIX)/include -DAP_ENABLE_CURL -ffunction-sections -fdata-sections
CURL_LDFLAGS := \
	-Wl,--whole-archive $(CURL_PREFIX)/lib/libcurl.a -Wl,--no-whole-archive \
	$(OPENSSL_LIBSSL) $(OPENSSL_LIBCRYPTO) \
	-lz -Wl,--gc-sections

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
	-mcpu=cortex-a53 -mtune=cortex-a53 \
	-DPLATFORM_TG5040 \
	-I/workspace/apostrophe/include \
	-I/workspace/src \
	$(SDL_CFLAGS) $(CURL_CFLAGS)

LDFLAGS := $(SDL_LDFLAGS) $(CURL_LDFLAGS) -lpthread -ldl -lm -lrt

.PHONY: all clean

all: $(BUILD_DIR)/somaplayer $(BUILD_DIR)/mpg123

$(CURL_STAMP): /workspace/apostrophe/scripts/build_third_party.sh
	$(THIRD_PARTY_SCRIPT) ensure-curl $(PLATFORM_ID)

$(MPG123_STAMP): $(MPG123_SCRIPT)
	$(MPG123_SCRIPT)

$(BUILD_DIR)/mpg123: $(MPG123_STAMP)
	cp $(MPG123_BIN) $@

$(BUILD_DIR)/somaplayer: $(SRCS) $(CURL_STAMP)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	$(THIRD_PARTY_SCRIPT) stage-runtime-libs $(PLATFORM_ID) "$@" "$(BUILD_DIR)/lib"

clean:
	rm -rf $(BUILD_DIR)
