# MM0 Makefile
#
# Build:   make              (builds mm0, NMP_TT_SF_v5, NMP_TT_SF_v5F, and NMP_TT_SF_v5D)
# Legacy:  make legacy       (builds ./mm0 for manual network matches)
# Batch:   make tournament   (builds ./NMP_TT_SF_v5, ./NMP_TT_SF_v5F, and ./NMP_TT_SF_v5D)
# Fast:    make fullspeed    (builds only ./NMP_TT_SF_v5F: no CSV writes)
# Deep:    make deep          (builds only ./NMP_TT_SF_v5D)
#          make DEEP_THINK_TIME_MS=30000 deep  (30s/move Deep build)
# Debug:   make DEBUG=1        (adds -DDEBUG -O0 -g, enables [DEBUG] logs)
# Clean:   make clean
#
# Requires: SDL, SDL_net, OpenMP, C++17 compiler.

CXX       ?= g++
CXXSTD    := -std=c++17
WARN      := -Wall -Wextra -Wno-unused-parameter

SDL_CFLAGS   := $(shell sdl-config --cflags)
SDL_LDFLAGS  := $(shell sdl-config --libs) -lSDL_net

OMP_FLAGS := -fopenmp

INCLUDES  := -I.

ifeq ($(DEBUG),1)
  OPT   := -O0 -g
  DEFS  := -DDEBUG
else
  OPT   := -O3 -march=native -DNDEBUG
  DEFS  :=
endif

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(DEFS) $(OMP_FLAGS) $(SDL_CFLAGS) $(INCLUDES)
LDFLAGS  := $(OMP_FLAGS) $(SDL_LDFLAGS) -pthread

SRCS := \
    main.cc \
    common/log.cc \
    rules/board.cc \
    time/clock.cc \
    strategy/strategy.cc \
    engine/engine.cc \
    engine/client.cc

OBJS := $(SRCS:.cc=.o)
DEPS := $(OBJS:.o=.d)
FULLSPEED_OBJS := $(SRCS:.cc=.fullspeed.o)
FULLSPEED_DEPS := $(FULLSPEED_OBJS:.o=.d)
DEEP_OBJS := $(SRCS:.cc=.deep.o)
DEEP_DEPS := $(DEEP_OBJS:.o=.d)

DEEP_THINK_TIME_MS ?= 20000

TARGET := mm0
ALGO_TARGET := NMP_TT_SF_v5
FULLSPEED_TARGET := mm0F
FULLSPEED_ALGO_TARGET := $(ALGO_TARGET)F
DEEP_TARGET := mm0D
DEEP_ALGO_TARGET := $(ALGO_TARGET)D

.PHONY: all legacy tournament fullspeed deep clean

all: $(TARGET) $(ALGO_TARGET) $(FULLSPEED_ALGO_TARGET) $(DEEP_ALGO_TARGET)

legacy: $(TARGET)

tournament: $(ALGO_TARGET) $(FULLSPEED_ALGO_TARGET) $(DEEP_ALGO_TARGET)

fullspeed: $(FULLSPEED_ALGO_TARGET)

deep: $(DEEP_ALGO_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

$(ALGO_TARGET): $(TARGET)
	cp $(TARGET) $(ALGO_TARGET)

$(FULLSPEED_TARGET): $(FULLSPEED_OBJS)
	$(CXX) $(FULLSPEED_OBJS) -o $@ $(LDFLAGS)

$(FULLSPEED_ALGO_TARGET): $(FULLSPEED_TARGET)
	cp $(FULLSPEED_TARGET) $(FULLSPEED_ALGO_TARGET)

$(DEEP_TARGET): $(DEEP_OBJS)
	$(CXX) $(DEEP_OBJS) -o $@ $(LDFLAGS)

$(DEEP_ALGO_TARGET): $(DEEP_TARGET)
	cp $(DEEP_TARGET) $(DEEP_ALGO_TARGET)

%.o: %.cc
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

%.fullspeed.o: %.cc
	$(CXX) $(CXXFLAGS) -DMM0_FULLSPEED_BUILD -MMD -MP -c $< -o $@

%.deep.o: %.cc
	$(CXX) $(CXXFLAGS) -DMM0_DEEP_BUILD -DMM0_DEEP_THINK_TIME_MS=$(DEEP_THINK_TIME_MS) -MMD -MP -c $< -o $@

-include $(DEPS) $(FULLSPEED_DEPS) $(DEEP_DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(FULLSPEED_OBJS) $(FULLSPEED_DEPS) $(DEEP_OBJS) $(DEEP_DEPS) $(TARGET) $(ALGO_TARGET) $(FULLSPEED_TARGET) $(FULLSPEED_ALGO_TARGET) $(DEEP_TARGET) $(DEEP_ALGO_TARGET)
