CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c99 -pthread
LDFLAGS = -pthread
TARGET  = vm

all: $(TARGET)

$(TARGET): vm.c
	$(CC) $(CFLAGS) -o $(TARGET) vm.c $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o correct.txt

.PHONY: all clean
