CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
SRC      = src/kv.c src/crc32.c
OBJ      = $(SRC:.c=.o)

all: minikv

minikv: $(OBJ) src/main.o
	$(CC) $(CFLAGS) -o $@ $^

tests/test_kv: $(OBJ) tests/test_kv.c
	$(CC) $(CFLAGS) -Isrc -o $@ $^

test: tests/test_kv
	./tests/test_kv

# Same tests under AddressSanitizer + UndefinedBehaviorSanitizer
sanitize:
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Isrc -o tests/test_kv_asan $(SRC) tests/test_kv.c
	./tests/test_kv_asan

%.o: %.c src/kv.h src/crc32.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o minikv tests/test_kv tests/test_kv_asan

.PHONY: all test sanitize clean
