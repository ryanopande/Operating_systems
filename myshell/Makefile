# myshell build — gcc, one object per source, then link

CC      = gcc
CFLAGS  = -Wall -Wextra -g
SRC     = src
BUILD   = build
TARGET  = myshell
OBJS    = $(BUILD)/errors.o $(BUILD)/parser.o $(BUILD)/executor.o $(BUILD)/main.o

all: $(TARGET) tests/hello

# step 4: link
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@

# step 1: error helpers
$(BUILD)/errors.o: $(SRC)/errors.c $(SRC)/errors.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# step 2: parser
$(BUILD)/parser.o: $(SRC)/parser.c $(SRC)/parser.h $(SRC)/errors.h $(SRC)/shell.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# step 3: executor and main loop
$(BUILD)/executor.o: $(SRC)/executor.c $(SRC)/executor.h $(SRC)/parser.h $(SRC)/errors.h $(SRC)/shell.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/main.o: $(SRC)/main.c $(SRC)/executor.h $(SRC)/parser.h $(SRC)/errors.h $(SRC)/shell.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

tests/hello: tests/hello.c
	$(CC) $(CFLAGS) $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD) $(TARGET) tests/hello

.PHONY: all run clean
