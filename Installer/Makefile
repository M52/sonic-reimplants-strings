CC ?= cc
CXX ?= c++
CFLAGS ?= -O2
CXXFLAGS ?= -O2
ARCH ?=

WP = 3rdparty/wavpack-5.9.0
IG = 3rdparty/imgui
WPSRC = $(addprefix $(WP)/src/,common_utils.c decorr_utils.c entropy_utils.c extra1.c extra2.c open_utils.c \
        open_filename.c open_legacy.c open_raw.c pack.c pack_dns.c pack_floats.c pack_utils.c read_words.c tags.c \
        tag_utils.c unpack.c unpack_floats.c unpack_seek.c unpack_utils.c write_words.c)
CSRC = $(WPSRC) 3rdparty/monocypher/monocypher.c
GUISRC = src/sr_main.cpp src/sr_gui.cpp $(IG)/imgui.cpp $(IG)/imgui_draw.cpp $(IG)/imgui_tables.cpp \
         $(IG)/imgui_widgets.cpp 3rdparty/imgui_sw/imgui_sw.cpp
INC = -I$(WP) -I$(WP)/src -I3rdparty/monocypher -I$(IG) -I3rdparty/imgui_sw -I3rdparty
DEFS = -DNDEBUG -DIMGUI_USE_BGRA_PACKED_COLOR

ifeq ($(shell uname -s),Darwin)
WINDOW = src/sr_window_macos.mm
LIBS = -framework Cocoa -framework Carbon
CLILIBS =
else
WINDOW = src/sr_window_x11.cpp
LIBS = -lX11 -lXext -lpthread -lm
CLILIBS = -lpthread -lm
endif

COBJ = $(patsubst %.c,obj/%.o,$(CSRC))
CORE = obj/src/sr_core.o
GUIOBJ = $(patsubst %.cpp,obj/%.o,$(GUISRC)) $(patsubst src/%.mm,obj/src/%.o,$(patsubst %.cpp,obj/%.o,$(WINDOW)))

sr_install: $(COBJ) $(CORE) $(GUIOBJ)
	$(CXX) $(ARCH) -o $@ $^ $(LIBS)

cli: $(COBJ) $(CORE) src/sr_main.cpp
	$(CXX) $(CXXFLAGS) $(ARCH) -std=c++17 -DSR_NO_GUI $(DEFS) $(INC) -o sr_install src/sr_main.cpp $(COBJ) $(CORE) $(CLILIBS)

obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ARCH) -std=gnu99 $(DEFS) $(INC) -c $< -o $@

obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(ARCH) -std=c++17 $(DEFS) $(INC) -c $< -o $@

obj/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(ARCH) -std=c++17 -fobjc-arc $(DEFS) $(INC) -c $< -o $@

clean:
	rm -rf obj sr_install

.PHONY: cli clean
