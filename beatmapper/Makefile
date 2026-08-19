.PHONY: all clean run

UNAME    := $(shell uname)
TARGET   := beatmapper

SRC_DIR    := src
VENDOR_DIR := vendor
IMGUI_DIR  := $(VENDOR_DIR)/imgui

SOURCES := \
    $(SRC_DIR)/main.cpp \
    $(SRC_DIR)/audio.cpp \
    $(SRC_DIR)/wsola.cpp \
    $(SRC_DIR)/pitch_node.cpp \
    $(SRC_DIR)/spectrogram.cpp \
    $(SRC_DIR)/editor.cpp \
    $(SRC_DIR)/beatmap.cpp \
    $(SRC_DIR)/sectionmap.cpp \
    $(SRC_DIR)/lyricmap.cpp \
    $(SRC_DIR)/undo.cpp \
    $(SRC_DIR)/recent.cpp \
    $(SRC_DIR)/ui_timeline.cpp \
    $(SRC_DIR)/ui_toolbar.cpp \
    $(SRC_DIR)/ui_dock.cpp \
    $(SRC_DIR)/panels.cpp \
    $(SRC_DIR)/ui_help.cpp \
    $(SRC_DIR)/ui_smoothing.cpp \
    $(SRC_DIR)/miscmap.cpp \
    $(SRC_DIR)/ui_annstrip.cpp \
    $(SRC_DIR)/ui_chroma.cpp \
    $(SRC_DIR)/chroma_algo.cpp \
    $(SRC_DIR)/beat_algo.cpp \
    $(SRC_DIR)/beat_spectral_flux.cpp \
    $(SRC_DIR)/ui_beat_detector.cpp \
    $(SRC_DIR)/chroma_goertzel.cpp \
    $(SRC_DIR)/chroma_hps.cpp \
    $(SRC_DIR)/chroma_nnls.cpp \
    $(SRC_DIR)/chroma_peaks.cpp \
    $(SRC_DIR)/chroma_resonate.cpp \
    $(IMGUI_DIR)/imgui.cpp \
    $(IMGUI_DIR)/imgui_demo.cpp \
    $(IMGUI_DIR)/imgui_draw.cpp \
    $(IMGUI_DIR)/imgui_tables.cpp \
    $(IMGUI_DIR)/imgui_widgets.cpp \
    $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
    $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

INCLUDES := \
    -I$(SRC_DIR) \
    -I$(IMGUI_DIR) \
    -I$(IMGUI_DIR)/backends \
    -I$(VENDOR_DIR)/miniaudio

# -MMD -MP emits .d files so edits to a header rebuild every .cpp that includes
# it (without this, stale objects link against an old struct layout).
DEPFLAGS := -MMD -MP

ifeq ($(UNAME), Darwin)
    CXX      := clang++
    CXXFLAGS := -std=c++17 -O2 -Wall -DGL_SILENCE_DEPRECATION
    # GLFW installed via homebrew
    GLFW_INC := -I/opt/homebrew/include
    GLFW_LIB := -L/opt/homebrew/lib -lglfw
    LDFLAGS  := $(GLFW_LIB) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -framework UniformTypeIdentifiers -framework CoreAudio -framework AudioToolbox
    INCLUDES += $(GLFW_INC)
    # macOS platform file (Objective-C++)
    PLATFORM_OBJ := $(SRC_DIR)/platform_macos.o
else
    CXX      := g++
    CXXFLAGS := -std=c++17 -O2 -Wall
    GLFW_FLAGS := $(shell pkg-config --cflags --libs glfw3 2>/dev/null || echo "-lglfw")
    GTK_FLAGS  := $(shell pkg-config --cflags --libs gtk+-3.0)
    LDFLAGS    := $(GLFW_FLAGS) $(GTK_FLAGS) -lGL -ldl -lpthread
    INCLUDES   += $(shell pkg-config --cflags gtk+-3.0)
    PLATFORM_OBJ := $(SRC_DIR)/platform_linux.o
endif

OBJECTS := $(SOURCES:.cpp=.o) $(PLATFORM_OBJ)
DEPS    := $(OBJECTS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# Objective-C++ rule for macOS platform file
$(SRC_DIR)/platform_macos.o: $(SRC_DIR)/platform_macos.mm
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -x objective-c++ -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TARGET) $(OBJECTS) $(DEPS)

run: all
	./$(TARGET)
