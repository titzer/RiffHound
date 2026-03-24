.PHONY: all clean run

UNAME    := $(shell uname)
TARGET   := beatmapper

SRC_DIR    := src
VENDOR_DIR := vendor
IMGUI_DIR  := $(VENDOR_DIR)/imgui

SOURCES := \
    $(SRC_DIR)/main.cpp \
    $(SRC_DIR)/audio.cpp \
    $(SRC_DIR)/spectrogram.cpp \
    $(SRC_DIR)/editor.cpp \
    $(SRC_DIR)/beatmap.cpp \
    $(SRC_DIR)/ui_timeline.cpp \
    $(SRC_DIR)/ui_toolbar.cpp \
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
    LDFLAGS  := $(GLFW_FLAGS) -lGL -ldl -lpthread
    PLATFORM_OBJ := $(SRC_DIR)/platform_linux.o
endif

OBJECTS := $(SOURCES:.cpp=.o) $(PLATFORM_OBJ)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Objective-C++ rule for macOS platform file
$(SRC_DIR)/platform_macos.o: $(SRC_DIR)/platform_macos.mm
	$(CXX) $(CXXFLAGS) $(INCLUDES) -x objective-c++ -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)

run: all
	./$(TARGET)
