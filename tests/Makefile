# 主机侧单元测试(不需要 Docker,也不需要 Switch)。
#
#   make -C tests          # 编译并运行
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
BINARY := host-tests

.PHONY: all run clean
all: run

run: $(BINARY)
	@./$(BINARY)

$(BINARY): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $@

clean:
	rm -f $(BINARY)
