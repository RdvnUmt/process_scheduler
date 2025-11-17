CC = gcc
CFLAGS = -Wall -Wextra -pthread -std=c99
TARGET = process_scheduler
SOURCES = process_scheduler.c

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

.PHONY: clean
