# Flashpoint PS5 - PS5 payload build (ps5-payload-sdk).
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make            -> build/ps5/flashpoint-ps5.elf
#   make homebrew   -> dist/FlashpointPS5/ (copy to /data/homebrew/)
#   make test       -> send the ELF to elfldr on $(PS5_HOST):$(PS5_PORT)
# The PC test build is Makefile.pc.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

PYTHON ?= python3
BUILD  := build/ps5
ELF    := $(BUILD)/flashpoint-ps5.elf

APP_SRCS := src/main.c src/util.c src/config.c src/httpd.c src/library.c \
            src/gamedata.c src/launch.c src/gameserver.c src/api.c \
            src/sync.c src/web.c \
            src/net_ps5.c src/platform_ps5.c
TP_SRCS  := third_party/sqlite/sqlite3.c third_party/miniz/miniz.c \
            third_party/cjson/cJSON.c third_party/sha256/sha256.c
GEN_SRC  := $(BUILD)/assets_gen.c
WWW      := $(shell find www -type f)

# no -g: debug info isn't used on the console and only makes the ELF bigger
CFLAGS   := -O2 -Wall -Werror
TPFLAGS  := -O2 -w -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
            -DSQLITE_DEFAULT_MEMSTATUS=0 -DHAVE_USLEEP=1
LDADD    := -lSceSystemService -lSceUserService -lSceSsl -lSceHttp -lSceNet -lpthread

APP_OBJS := $(APP_SRCS:%.c=$(BUILD)/%.o)
TP_OBJS  := $(TP_SRCS:%.c=$(BUILD)/%.o)

all: $(ELF)

$(BUILD)/src/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TPFLAGS) -c -o $@ $<

$(GEN_SRC): tools/embed.py $(WWW)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/embed.py www $@

$(BUILD)/assets_gen.o: $(GEN_SRC)
	$(CC) -O1 -w -Isrc -c -o $@ $<

$(ELF): $(APP_OBJS) $(TP_OBJS) $(BUILD)/assets_gen.o
	$(CC) -o $@ $^ $(LDADD)

homebrew: $(ELF)
	rm -rf dist/FlashpointPS5
	mkdir -p dist/FlashpointPS5
	cp -r homebrew/FlashpointPS5/. dist/FlashpointPS5/
	cp $(ELF) dist/FlashpointPS5/flashpoint-ps5.elf

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -rf $(BUILD) dist/FlashpointPS5

.PHONY: all homebrew test clean
