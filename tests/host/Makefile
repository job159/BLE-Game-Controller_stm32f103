# 主機端單元測試（僅編譯無硬體相依的 mw 模組）
# 使用：make && ./run_tests   （Windows 可用 MinGW / MSYS2 的 make+gcc）

CC      ?= gcc
ROOT    := ../..
CFLAGS  := -std=c11 -Wall -Wextra -O0 -g -I$(ROOT)/Core/Inc

SRCS := test_main.c \
        $(ROOT)/Core/Src/mw/ringbuf.c \
        $(ROOT)/Core/Src/mw/crc16.c \
        $(ROOT)/Core/Src/mw/proto.c \
        $(ROOT)/Core/Src/mw/button.c \
        $(ROOT)/Core/Src/mw/xutil.c

run_tests: $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

.PHONY: test clean
test: run_tests
	./run_tests

clean:
	rm -f run_tests run_tests.exe
