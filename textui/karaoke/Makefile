# The terminal karaoke client.  The audio engine is beatmapper's, compiled
# from its sources in place (into build/ here, so beatmapper's own objects are
# never touched), so speed and pitch arrive with it rather than as a copy.

BEATMAPPER := ../../beatmapper
BM_SRC     := $(BEATMAPPER)/src
BM_VENDOR  := $(BEATMAPPER)/vendor

CXX      := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -MMD -MP -DSIGNALSMITH_USE_ACCELERATE \
            -I src -I $(BM_SRC) -I $(BM_VENDOR)/miniaudio -I $(BM_VENDOR)
LDFLAGS  := -framework CoreAudio -framework AudioToolbox -framework CoreFoundation \
            -framework Accelerate

SRCS    := $(wildcard src/*.cpp)
BM_SRCS := $(BM_SRC)/audio.cpp $(BM_SRC)/stretch.cpp
OBJS    := $(SRCS:src/%.cpp=build/%.o) $(BM_SRCS:$(BM_SRC)/%.cpp=build/bm/%.o)

karaoke: $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/bm/%.o: $(BM_SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

clean:
	rm -rf build karaoke

.PHONY: clean
-include $(OBJS:.o=.d)
