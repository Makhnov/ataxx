CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

SDL_CFLAGS := $(shell sdl-config --cflags)
SDL_LIBS := $(shell sdl-config --libs)

COMMON := launchStrategy.cc bidiarray.cc shmem.cc strategy_compat.cc
BUILD := build

.PHONY: all greedy minimax alphabeta clean

all: greedy minimax alphabeta

$(BUILD):
	mkdir -p $(BUILD)

greedy: | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -I. $(COMMON) ../strategies/strategy_glouton.cc -o $(BUILD)/greedy-core $(SDL_LIBS)

minimax: | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -I. $(COMMON) ../strategies/strategy_minimax.cc -o $(BUILD)/minimax-core $(SDL_LIBS)

alphabeta: | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -I. $(COMMON) ../strategies/strategy_AB_ID.cc -o $(BUILD)/alphabeta-core $(SDL_LIBS)

clean:
	rm -rf $(BUILD)
