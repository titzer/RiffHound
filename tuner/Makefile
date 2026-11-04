.PHONY: all clean run selftest install

UNAME  := $(shell uname)
TARGET := tuner

SRC_DIR    := src
# Shares beatmapper's vendored Dear ImGui and miniaudio.
VENDOR_DIR := ../beatmapper/vendor
IMGUI_DIR  := $(VENDOR_DIR)/imgui
BUILD      := build

SOURCES := \
    $(SRC_DIR)/main.cpp \
    $(SRC_DIR)/ui.cpp \
    $(SRC_DIR)/pitch.cpp \
    $(SRC_DIR)/audio_io.cpp \
    $(SRC_DIR)/settings.cpp \
    $(IMGUI_DIR)/imgui.cpp \
    $(IMGUI_DIR)/imgui_draw.cpp \
    $(IMGUI_DIR)/imgui_tables.cpp \
    $(IMGUI_DIR)/imgui_widgets.cpp \
    $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
    $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

INCLUDES := -I$(SRC_DIR) -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(VENDOR_DIR)/miniaudio
DEPFLAGS := -MMD -MP

ifeq ($(UNAME), Darwin)
    CXX      := clang++
    CXXFLAGS := -std=c++17 -O2 -Wall -DGL_SILENCE_DEPRECATION
    INCLUDES += -I/opt/homebrew/include
    LDFLAGS  := -L/opt/homebrew/lib -lglfw -framework OpenGL -framework Cocoa -framework IOKit \
                -framework CoreVideo -framework CoreAudio -framework AudioToolbox
    PLATFORM_SRC := $(SRC_DIR)/platform_macos.mm
else
    CXX      := g++
    CXXFLAGS := -std=c++17 -O2 -Wall
    LDFLAGS  := $(shell pkg-config --libs glfw3 2>/dev/null || echo "-lglfw") -lGL -ldl -lpthread -lm
    PLATFORM_SRC := $(SRC_DIR)/platform_linux.cpp
endif

# Objects go under build/ so the shared vendor tree stays clean.
OBJECTS := $(patsubst %,$(BUILD)/%.o,$(notdir $(basename $(SOURCES) $(PLATFORM_SRC))))
DEPS    := $(OBJECTS:.o=.d)
vpath %.cpp $(SRC_DIR) $(IMGUI_DIR) $(IMGUI_DIR)/backends
vpath %.mm  $(SRC_DIR)

APP     := Tuner.app
APP_BIN := $(APP)/Contents/MacOS/Tuner

ifeq ($(UNAME), Darwin)
all: $(APP)
else
all: $(TARGET)
endif

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.mm | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -x objective-c++ -fobjc-arc -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

-include $(DEPS)

# The real binary lives inside the bundle so the microphone permission and
# Dock icon belong to Tuner.app whichever way it is launched; ~/bin/tuner is
# a symlink to it (make install).
$(APP): $(TARGET) Info.plist $(APP)/Contents/Resources/Tuner.icns
	mkdir -p $(APP)/Contents/MacOS
	cp Info.plist $(APP)/Contents/Info.plist
	cp $(TARGET) $(APP_BIN)
	codesign --force --sign - $(APP) 2>/dev/null || true
	touch $(APP)
	@echo "built $(APP)"

$(APP)/Contents/Resources/Tuner.icns: scripts/make-icon.py
	mkdir -p $(APP)/Contents/Resources
	python3 scripts/make-icon.py $(BUILD)/Tuner.iconset
	iconutil -c icns $(BUILD)/Tuner.iconset -o $@
	rm -rf $(BUILD)/Tuner.iconset

install: all
	mkdir -p $(HOME)/bin
	ln -sf "$(CURDIR)/$(APP_BIN)" $(HOME)/bin/tuner
	@echo "linked ~/bin/tuner -> $(CURDIR)/$(APP_BIN)"

run: all
	./$(APP_BIN)

selftest: $(TARGET)
	./$(TARGET) --selftest

clean:
	rm -rf $(BUILD) $(TARGET) $(APP)
