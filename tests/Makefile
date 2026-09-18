# Host-side unit tests: no Docker, no Switch needed.
#
#   make -C tests          # build and run
#   make -C tests clean
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
INCLUDES := -I../source
SOURCES := host_tests.cpp \
           ../source/env/config_ini.cpp \
           ../source/i18n/strings.cpp \
           ../source/manifest/json.cpp \
           ../source/manifest/manifest.cpp \
           ../source/install/gate.cpp \
           ../source/util/sha256.cpp \
           ../source/util/time.cpp
# Headers are dependencies too: without this, changing a header alone reused the previous
# binary, so the tests could "pass" against code that no longer exists.
HEADERS := $(shell find ../source -name '*.hpp')
BINARY := host-tests

.PHONY: all run clean check-i18n
all: run

run: check-i18n $(BINARY)
	@./$(BINARY)

# Text() maps ids to rows by index, so the enum and the table order have to agree; nothing in
# C++ notices a swap (it shows the wrong string at runtime), so check it here.
check-i18n:
	@python3 ../tools/check-i18n.py

$(BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $@

clean:
	rm -f $(BINARY)
