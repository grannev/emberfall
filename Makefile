CC = gcc
PKG_CONFIG ?= pkg-config

APP := emberfall
WORLD_SOURCES := src/materials.c src/world_storage.c src/world_simulation.c \
	src/world_thermal.c src/world_generation.c src/world_lighting.c \
	src/world_effects.c src/material_render.c src/world_render_data.c \
	src/world_components.c
SOURCES := src/main.c src/game.c src/game_events.c src/input.c \
	$(WORLD_SOURCES) \
	src/renderer.c src/world_renderer.c src/presentation_fx.c \
	src/presentation_fx_renderer.c src/terrain_body_render_data.c \
	src/terrain_body_renderer.c \
	src/player.c src/player_renderer.c src/abilities.c src/ability_renderer.c \
	src/dynamic_terrain.c src/terrain_extraction.c src/terrain_physics.c \
	src/particles.c src/particle_renderer.c src/audio.c
# The headless suite links CPU-side gameplay only: no window or GL context.
TEST_APP := emberfall-tests
TEST_SOURCES := tests/world_tests.c src/game.c src/game_events.c \
	$(WORLD_SOURCES) \
	src/player.c src/abilities.c src/particles.c src/dynamic_terrain.c \
	src/terrain_extraction.c src/terrain_physics.c src/presentation_fx.c \
	src/terrain_body_render_data.c
BENCH_APP := emberfall-bench
BENCH_SOURCES := bench/benchmark.c $(WORLD_SOURCES) src/player.c \
	src/dynamic_terrain.c src/terrain_physics.c
HEADERS := $(wildcard src/*.h)
CONFIG ?= release
RUN_ARGS ?=
BENCH_ARGS ?=
BUILD_DIR := build/$(CONFIG)
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
TARGET := $(BUILD_DIR)/$(APP)
TEST_TARGET := $(BUILD_DIR)/$(TEST_APP)
BENCH_TARGET := $(BUILD_DIR)/$(BENCH_APP)

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags raylib 2>/dev/null)
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion
LDLIBS += $(shell $(PKG_CONFIG) --libs raylib 2>/dev/null) -lm

ifeq ($(CONFIG),debug)
    CFLAGS += $(CFLAGS_COMMON) -g -O0
else ifeq ($(CONFIG),asan)
    CFLAGS += $(CFLAGS_COMMON) -g -O1 -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address
else ifeq ($(CONFIG),ubsan)
    CFLAGS += $(CFLAGS_COMMON) -g -O1 -fsanitize=undefined -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=undefined
else ifeq ($(CONFIG),profile)
    CFLAGS += $(CFLAGS_COMMON) -g -O2 -pg
    LDFLAGS += -pg
else
    CFLAGS += $(CFLAGS_COMMON) -O2
endif

.PHONY: all run debug clean check-raylib test bench asan ubsan profile \
	compile_commands.json

all: check-raylib $(TARGET)

test: check-raylib $(TEST_TARGET)
	./$(TEST_TARGET)

bench: check-raylib $(BENCH_TARGET)
	./$(BENCH_TARGET) $(BENCH_ARGS)

check-raylib:
	@$(PKG_CONFIG) --exists raylib || \
		{ echo "error: raylib was not found by pkg-config"; \
		  echo "install raylib and pkgconf, then run make again"; exit 1; }

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_TARGET): $(TEST_SOURCES) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BENCH_TARGET): $(BENCH_SOURCES) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(BENCH_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

run: all
	./$(TARGET) $(RUN_ARGS)

debug:
	$(MAKE) CONFIG=debug all

asan:
	$(MAKE) CONFIG=asan all
	$(MAKE) CONFIG=asan test

ubsan:
	$(MAKE) CONFIG=ubsan all
	$(MAKE) CONFIG=ubsan test

profile:
	$(MAKE) CONFIG=profile all

# clangd and other tooling read this; regenerating it is cheap and needs no
# extra dependency, because every translation unit is compiled the same way.
compile_commands.json:
	@printf '[\n' > $@
	@first=1; for source in $(SOURCES) $(TEST_SOURCES) $(BENCH_SOURCES); do \
		case " $$seen " in *" $$source "*) continue;; esac; \
		seen="$$seen $$source"; \
		if [ $$first -eq 0 ]; then printf ',\n' >> $@; fi; first=0; \
		printf '  {"directory": "%s", "file": "%s", "command": "$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -c %s"}' \
			"$(CURDIR)" "$$source" "$$source" >> $@; \
	done
	@printf '\n]\n' >> $@
	@echo "wrote $@"

clean:
	rm -rf build compile_commands.json

-include $(OBJECTS:.o=.d)
