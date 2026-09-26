.PHONY: all build run clean dev help

APP_NAME  ?= my_tui_app
BUILD_DIR ?= build

# Tự động gom các file .go để Make kiểm tra dependency chính xác
SRCS := $(shell find . -type f -name '*.go')

all: build

# Chỉ rebuild khi thực sự có file .go thay đổi
$(BUILD_DIR)/$(APP_NAME): $(SRCS)
	@mkdir -p $(BUILD_DIR)
	@go build -o $@ .

build: $(BUILD_DIR)/$(APP_NAME)

# Rebuild (nếu có thay đổi) rồi mới chạy
run: build
	@./$(BUILD_DIR)/$(APP_NAME)

# Live-reload khi code TUI (cần cài air: go install github.com/air-verse/air@latest)
dev:
	@if command -v air > /dev/null; then \
		air; \
	else \
		echo "Chưa cài air. Đang chạy fallback bằng 'make run'..."; \
		make run; \
	fi

clean:
	@rm -rf $(BUILD_DIR)

help:
	@echo "Các lệnh hỗ trợ:"
	@echo "  make build  - Build binary vào $(BUILD_DIR)/"
	@echo "  make run    - Rebuild (nếu cần) và chạy ứng dụng"
	@echo "  make dev    - Chạy live-reload (tự rebuild khi sửa file)"
	@echo "  make clean  - Xóa thư mục build"
