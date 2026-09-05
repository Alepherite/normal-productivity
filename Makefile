TARGET = normal-productivity
CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra
LIBS = -lncursesw -lsqlite3

SRC = src/main.cpp
BUILD_DIR = build

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BUILD_DIR)/$(TARGET) $(LIBS)

run: $(BUILD_DIR)/$(TARGET)
	@./$(BUILD_DIR)/$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run clean
