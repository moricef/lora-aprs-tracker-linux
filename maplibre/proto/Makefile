# Prototype MapLibre texture + LVGL GL window.
# LVGL is rebuilt here with its own conf (OPENGLES/GLFW on) — the tracker
# build and its lv_conf.h are untouched.

ROOT    := ../..
ML      := /home/fab2/Developpement/LoRa_APRS/maplibre-native
MLBUILD := $(ML)/build-linux-opengl

CXX := g++
CC  := gcc
LVCONF := -DLV_CONF_PATH='"$(abspath lv_conf_proto.h)"'
OPT := -O2 -g

INC := -I$(ROOT)/lib/lvgl -I$(ROOT)/lib/lvgl/src/drivers/opengles/glad/include
INC += -I$(ML)/include -I$(ML)/platform/default/include
INC += -I$(ML)/vendor/maplibre-native-base/include
INC += $(foreach d,$(wildcard $(ML)/vendor/maplibre-native-base/deps/*/include),-I$(d))
INC += -I/usr/include/libdrm
INC += $(shell pkg-config --cflags glfw3) $(shell pkg-config --cflags freetype2)

LVGL_C := $(shell find $(ROOT)/lib/lvgl/src -name '*.c' | grep -v '/sdl/' | grep -v lv_draw_vector | sort)
LVGL_O := $(patsubst $(ROOT)/lib/lvgl/%.c,obj/lvgl/%.o,$(LVGL_C))

LIBS  = $(MLBUILD)/libmbgl-core.a
LIBS += $(MLBUILD)/libmbgl-vendor-parsedate.a
LIBS += $(MLBUILD)/vendor/maplibre-tile-spec/cpp/libmlt-cpp.a
LIBS += $(MLBUILD)/libmbgl-vendor-csscolorparser.a
LIBS += $(MLBUILD)/libmbgl-harfbuzz.a
LIBS += $(MLBUILD)/libmbgl-freetype.a
LIBS += $(MLBUILD)/libmbgl-vendor-nunicode.a
LIBS += $(MLBUILD)/libmbgl-vendor-sqlite.a

LDFLAGS  = $(shell pkg-config --libs glfw3)
LDFLAGS += -lEGL -lGLESv2 -lcurl -ljpeg -lpng -lz -lwebp -luv
LDFLAGS += -licuuc -licui18n -licudata -lsqlite3 -ldrm -lfreetype -lpthread -lrt -ldl

proto: obj/proto_main.o $(LVGL_O)
	$(CXX) $^ $(LIBS) $(LIBS) -o $@ $(LDFLAGS)

obj/proto_main.o: proto_main.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=gnu++20 $(OPT) -fno-rtti $(LVCONF) -DLV_CONF_INCLUDE_SIMPLE $(INC) -MMD -MP -c $< -o $@

obj/lvgl/%.o: $(ROOT)/lib/lvgl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(LVCONF) $(INC) -MMD -MP -c $< -o $@

clean:
	rm -rf obj proto

-include $(shell find obj -name '*.d' 2>/dev/null)

.PHONY: clean
