CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -pedantic -std=c99 -O2
LDFLAGS ?= -lpthread

SRC     := src/main.c src/server.c src/mime.c
OBJ     := $(SRC:.c=.o)
TARGET  := ohttpd

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)
