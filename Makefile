CC    = gcc
UNAME := $(shell uname -s)

# Homebrew header/library search paths are only relevant on macOS.
# On Linux the libraries live in the system default paths (no extra flags needed).
ifeq ($(UNAME), Darwin)
    BREW_INC = -I/opt/homebrew/include -I/usr/local/include
    BREW_LIB = -L/opt/homebrew/lib     -L/usr/local/lib
else
    BREW_INC =
    BREW_LIB =
endif

CFLAGS = -I. $(BREW_INC)

all:
	$(CC) $(CFLAGS) clients/admin_client.c -o bin/admin_client -lncurses
	$(CC) $(CFLAGS) clients/client_inet.c  -o bin/client_inet  -lncurses -lpthread
	$(CC) $(CFLAGS) servers/server_inet.c  -o bin/server $(BREW_LIB) -lpthread -lconfig -luv

clean:
	rm -f bin/admin_client bin/client_inet bin/server
	rm -fr *.o *.txt

format:
	clang-format -i clients/admin_client.c
	clang-format -i clients/client_inet.c
	clang-format -i servers/server_inet.c
	autopep8 --in-place clients/client_inet.py