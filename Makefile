CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -pedantic -std=c99 -O2 \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
           -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS ?= -lpthread

# PIE is enabled by default on most modern toolchains, but
# we ensure it explicitly so the binary benefits from ASLR.
export CFLAGS LDFLAGS

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
