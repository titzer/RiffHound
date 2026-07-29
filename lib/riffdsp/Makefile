.PHONY: all clean check

TARGET := riffdsp

# The DSP is compiled straight out of the beatmapper tree.  Those files include
# only their own headers plus libc/libm — no ImGui, no GL — so they are already
# library code in everything but packaging.  Compiling them here rather than
# copying them keeps exactly one implementation of beat detection and chroma.
BM_SRC := ../../beatmapper/src

SOURCES := \
    main.cpp \
    timeseries.cpp \
    audio_io.cpp \
    chords.cpp \
    $(BM_SRC)/beat_algo.cpp \
    $(BM_SRC)/beat_spectral_flux.cpp \
    $(BM_SRC)/chroma_algo.cpp \
    $(BM_SRC)/chroma_goertzel.cpp \
    $(BM_SRC)/chroma_hps.cpp \
    $(BM_SRC)/chroma_nnls.cpp \
    $(BM_SRC)/chroma_peaks.cpp

INCLUDES := -I. -I$(BM_SRC)

UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
    CXX := clang++
else
    CXX := g++
endif
CXXFLAGS := -std=c++17 -O2 -Wall
DEPFLAGS := -MMD -MP

OBJECTS := $(SOURCES:.cpp=.o)
DEPS    := $(OBJECTS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TARGET) $(OBJECTS) $(DEPS)

check: $(TARGET)
	./$(TARGET) info "$(HOME)/RiffHound/playalong/The Rover.mp3"
