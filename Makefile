CC = gcc
PKG_CONFIG ?= pkg-config

APP := emberfall
SOURCES := src/main.c src/world.c src/player.c src/powers.c src/particles.c
CONFIG ?= release
RUN_ARGS ?=
BUILD_DIR := build/$(CONFIG)
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
TARGET := $(BUILD_DIR)/$(APP)

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags raylib 2>/dev/null)
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs raylib 2>/dev/null) -lm

ifeq ($(CONFIG),debug)
    CFLAGS += $(CFLAGS_COMMON) -g -O0
else
    CFLAGS += $(CFLAGS_COMMON) -O2
endif

.PHONY: all run debug clean check-raylib

all: check-raylib $(TARGET)

check-raylib:
	@$(PKG_CONFIG) --exists raylib || \
		{ echo "error: raylib was not found by pkg-config"; \
		  echo "install raylib and pkgconf, then run make again"; exit 1; }

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

run: all
	./$(TARGET) $(RUN_ARGS)

debug:
	$(MAKE) CONFIG=debug all

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
