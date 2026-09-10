# Examples for the C++23 low-latency trading series.
#
#   make            build everything
#   make run        build and run everything
#   make asm=1 ...  also emit Intel-syntax assembly next to each binary

CXX      ?= c++
STD      := -std=c++23
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion
OPT      := -O2 -g -DNDEBUG
LDFLAGS  := -pthread

# -march=native is a large free win and a large portability hazard.
# Set NATIVE=0 when the build host differs from the run host.
# Apple clang on arm64 does not accept -march=native.
NATIVE   ?= $(shell uname -s | grep -q Darwin && echo 0 || echo 1)
ifeq ($(NATIVE),1)
  ARCH := -march=native
else
  ARCH :=
endif

# Some macOS Command Line Tools installs ship libc++ only inside the SDK, and
# clang does not add that path itself. Detect it rather than fail on <algorithm>.
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
  ifneq ($(wildcard $(SDK)/usr/include/c++/v1/algorithm),)
    SYSINC := -isystem $(SDK)/usr/include/c++/v1
  endif
endif

CXXFLAGS := $(STD) $(WARN) $(OPT) $(ARCH) $(SYSINC)

BINS := cache_effects spsc_demo alloc_guard_demo

all: $(BINS)

%: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

run: all
	@for b in $(BINS); do \
	  echo "=== $$b ==="; ./$$b; echo; \
	done

asm: cache_effects.cpp
	$(CXX) $(CXXFLAGS) -S -masm=intel -o cache_effects.s cache_effects.cpp

clean:
	rm -f $(BINS) *.s

.PHONY: all run asm clean
