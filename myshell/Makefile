# myshell build — gcc, one object per source, then link (myshell, server, client)

CC      = gcc
CFLAGS  = -Wall -Wextra -g
SRC     = src
BUILD   = build
TARGET  = myshell

# objects every program needs to parse and run a command line (Phase 1 engine)
CORE_OBJS   = $(BUILD)/errors.o $(BUILD)/parser.o $(BUILD)/executor.o $(BUILD)/builtins.o

# phase 1: local shell
SHELL_OBJS  = $(CORE_OBJS) $(BUILD)/lineio.o $(BUILD)/main.o

# phase 2: server (runs commands) and client (prompt + display)
SERVER_OBJS = $(CORE_OBJS) $(BUILD)/net.o $(BUILD)/server.o
CLIENT_OBJS = $(BUILD)/errors.o $(BUILD)/lineio.o $(BUILD)/net.o $(BUILD)/client.o

# every header; objects are rebuilt when any of them changes, which keeps the rules short
HEADERS     = $(wildcard $(SRC)/*.h)

all: $(TARGET) server client tests/hello

# link step for each program
$(TARGET): $(SHELL_OBJS)
	$(CC) $(SHELL_OBJS) -o $@

server: $(SERVER_OBJS)
	$(CC) $(SERVER_OBJS) -o $@

client: $(CLIENT_OBJS)
	$(CC) $(CLIENT_OBJS) -o $@

# compile step: one object per source file, shared by every program that uses it
$(BUILD)/%.o: $(SRC)/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

tests/hello: tests/hello.c
	$(CC) $(CFLAGS) $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD) $(TARGET) server client tests/hello

.PHONY: all run clean