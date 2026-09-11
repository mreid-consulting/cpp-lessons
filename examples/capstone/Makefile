# Capstone tick-to-trade skeleton.

CXX      ?= c++
STD      := -std=c++23
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion
OPT      := -O2 -g -DNDEBUG
LDFLAGS  := -pthread

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
  ifneq ($(wildcard $(SDK)/usr/include/c++/v1/algorithm),)
    SYSINC := -isystem $(SDK)/usr/include/c++/v1
  endif
  NATIVE ?= 0
else
  NATIVE ?= 1
endif

ifeq ($(NATIVE),1)
  ARCH := -march=native
else
  ARCH :=
endif

CXXFLAGS := $(STD) $(WARN) $(OPT) $(ARCH) $(SYSINC)
HEADERS  := types.hpp spsc.hpp book.hpp decoder.hpp strategy.hpp risk.hpp \
            feed.hpp ../bench.hpp ../spsc_queue.hpp ../trace_ring.hpp

# Set TRACE=0 to compile the ring tracer out and measure what it cost.
TRACE    ?= 1

trader: main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) main.cpp -o trader $(LDFLAGS)

trace_demo: trace_demo.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -DHFT_TRACE=$(TRACE) trace_demo.cpp -o trace_demo $(LDFLAGS)

run: trader trace_demo
	./trader
	@echo
	./trace_demo

asm: main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -S -masm=intel -o trader.s main.cpp

clean:
	rm -f trader trace_demo trader.s

.PHONY: run asm clean
