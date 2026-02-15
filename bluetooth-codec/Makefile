CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -lm
TARGET = nanocodec

SRCS = main.c nanocodec.c wav.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) -o $@ $^ $(CFLAGS)

clean:
	rm -f $(TARGET) $(OBJS) *.nc1 test_output.wav

.PHONY: all clean
