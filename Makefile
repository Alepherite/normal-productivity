TARGET = normal-productivity
CXX = g++
CXXFLAGS = -std=c++17 -O3 -march=native -flto -Wall -Wextra -Wpedantic -D_XOPEN_SOURCE_EXTENDED
LDFLAGS = -flto
LIBS = -lncursesw -lsqlite3

SRC = src/main.cpp
BUILD_DIR = build

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BUILD_DIR)/$(TARGET) $(LDFLAGS) $(LIBS)

run: $(BUILD_DIR)/$(TARGET)
	@./$(BUILD_DIR)/$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run clean
