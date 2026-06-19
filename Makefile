TARGET := travelpi
BUILD_DIR := build
SRC_DIR := src
INC_DIR := include

TRAVEL_CONFIG_MANIFEST := $(if $(wildcard config/trips.json),config/trips.json,config/trips.example.json)
GENERATED_CONFIG := $(SRC_DIR)/travel_config.c

SRC := $(SRC_DIR)/main.c $(GENERATED_CONFIG)
OBJ := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

CC ?= cc
PLATFORM ?= desktop
BUILD ?= release
UNAME_S := $(shell uname -s)
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)

CPPFLAGS += -I$(INC_DIR)
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections
LDLIBS += -lm

RAYLIB_CFLAGS ?= $(shell pkg-config --cflags raylib 2>/dev/null)

ifeq ($(UNAME_S),Darwin)
	LDFLAGS += -Wl,-dead_strip
	ifeq ($(strip $(RAYLIB_CFLAGS)),)
		ifneq ($(BREW_PREFIX),)
			RAYLIB_CFLAGS += -I$(BREW_PREFIX)/include
		endif
	endif
else
	LDFLAGS += -Wl,--gc-sections
endif

ifeq ($(BUILD),debug)
	CFLAGS += -O0 -g3 -DTRAVELPI_DEBUG=1
else
	CFLAGS += -Os -DNDEBUG
	ifneq ($(UNAME_S),Darwin)
		LDFLAGS += -s
	endif
endif

ifeq ($(PLATFORM),pi)
	CPPFLAGS += -DTRAVELPI_DRM=1 -DPLATFORM_DRM
	RAYLIB_LIBS ?= $(shell pkg-config --libs raylib 2>/dev/null || printf '%s' '-lraylib -lGLESv2 -lEGL -ldrm -lgbm -lpthread -ldl -lrt')
else
	ifeq ($(UNAME_S),Darwin)
		RAYLIB_LIBS ?= $(shell pkg-config --libs raylib 2>/dev/null || printf '%s' '-L$(BREW_PREFIX)/lib -lraylib -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo')
	else
		RAYLIB_LIBS ?= $(shell pkg-config --libs raylib 2>/dev/null || printf '%s' '-lraylib -lm -lpthread -ldl -lrt')
	endif
endif

CPPFLAGS += $(RAYLIB_CFLAGS)
LDLIBS += $(RAYLIB_LIBS)

.PHONY: all clean run debug pi print-flags

all: $(BUILD_DIR)/$(TARGET)

debug:
	$(MAKE) BUILD=debug

pi:
	$(MAKE) PLATFORM=pi BUILD=release

run: $(BUILD_DIR)/$(TARGET)
	./$(BUILD_DIR)/$(TARGET) --windowed --show-fps

print-flags:
	@printf 'CPPFLAGS=%s\nCFLAGS=%s\nLDFLAGS=%s\nLDLIBS=%s\n' "$(CPPFLAGS)" "$(CFLAGS)" "$(LDFLAGS)" "$(LDLIBS)"

$(BUILD_DIR)/$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(GENERATED_CONFIG): $(TRAVEL_CONFIG_MANIFEST) scripts/generate_travel_config.py
	python3 scripts/generate_travel_config.py --manifest $(TRAVEL_CONFIG_MANIFEST) --output $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEP)
