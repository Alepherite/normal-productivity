.PHONY: all build run clean dev help

APP_NAME  ?= my_tui_app
BUILD_DIR ?= build

# Automatically find all .go files for accurate dependency tracking
SRCS := $(shell find . -type f -name '*.go')

all: build

# Only rebuild when .go files actually change
$(BUILD_DIR)/$(APP_NAME): $(SRCS)
	@mkdir -p $(BUILD_DIR)
	@go build -o $@ .

build: $(BUILD_DIR)/$(APP_NAME)

# Rebuild (if changed) then run
run: build
	@./$(BUILD_DIR)/$(APP_NAME)

# Live-reload for TUI development (requires air: go install github.com/air-verse/air@latest)
dev:
	@if command -v air > /dev/null; then \
		air; \
	else \
		echo "air is not installed. Falling back to 'make run'..."; \
		make run; \
	fi

clean:
	@rm -rf $(BUILD_DIR)

help:
	@echo "Available commands:"
	@echo "  make build  - Build binary into $(BUILD_DIR)/"
	@echo "  make run    - Rebuild (if needed) and run the application"
	@echo "  make dev    - Run live-reload (auto-rebuilds on file changes)"
	@echo "  make clean  - Remove the build directory"
