# Host-side unit tests: no Docker, no Switch needed.
#
#   make -C tests          # build and run
#   make -C tests clean
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
INCLUDES := -I../source -I../source/third_party/qrcodegen
SOURCES := host_tests.cpp \
           ../source/env/cheat_file.cpp \
           ../source/env/config_ini.cpp \
           ../source/i18n/strings.cpp \
           ../source/util/json.cpp \
           ../source/manifest/manifest.cpp \
           ../source/install/gate.cpp \
           ../source/install/container.cpp \
           ../source/install/install_task.cpp \
           ../source/install/nca.cpp \
           ../source/install/package.cpp \
           ../source/install/icon_file.cpp \
           ../source/install/title_name.cpp \
           ../source/util/qr_code.cpp \
           ../source/ui/settings.cpp \
           ../source/util/sha256.cpp \
           ../source/util/time.cpp
# Headers are dependencies too: without this, changing a header alone reused the previous
# binary, so the tests could "pass" against code that no longer exists.
HEADERS := $(shell find ../source -name '*.hpp')
BINARY := host-tests
# Vendored C (qrcodegen) is compiled as C, not as C++ -- the same way the NRO build compiles it.
# It keeps the upstream warning level (no -Werror): we do not edit third-party sources, so a
# warning here is upstream's to fix, not a reason to block our build.
CC ?= cc
CFLAGS ?= -std=c99 -O1 -g -Wall -Wextra -I../source/third_party/qrcodegen
VENDOR_OBJ := qrcodegen.o

.PHONY: all run clean check-i18n
all: run

run: check-i18n $(BINARY)
	@./$(BINARY)

# Text() maps ids to rows by index, so the enum and the table order have to agree; nothing in
# C++ notices a swap (it shows the wrong string at runtime), so check it here.
check-i18n:
	@python3 ../tools/check-i18n.py

$(BINARY): $(SOURCES) $(HEADERS) $(VENDOR_OBJ)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) $(VENDOR_OBJ) -o $@

$(VENDOR_OBJ): ../source/third_party/qrcodegen/qrcodegen.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(BINARY) $(VENDOR_OBJ)
