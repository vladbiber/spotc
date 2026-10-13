CC      ?= cc
PKGS     = ncursesw json-c libcurl openssl
CFLAGS  += -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE $(shell pkg-config --cflags $(PKGS))
LDLIBS   = $(shell pkg-config --libs $(PKGS))
PREFIX  ?= $(HOME)/.local

OBJ = src/main.o src/ui.o src/api.o src/auth.o src/player.o src/config.o src/local.o

all: spotc spotc-fx spotc-ipv4.so spotc-reverse-cache

spotc: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): src/spotc.h

spotc-fx: src/fx.c src/vinyl.h src/rain.h
	$(CC) $(CFLAGS) -o $@ src/fx.c -lm

spotc-ipv4.so: src/ipv4.c
	$(CC) -O2 -shared -fPIC -o $@ src/ipv4.c -ldl

spotc-reverse-cache: token-helper/src/bin/spotc-reverse-cache.rs token-helper/Cargo.toml
	cargo build --release --manifest-path token-helper/Cargo.toml --bin spotc-reverse-cache
	cp token-helper/target/release/spotc-reverse-cache $@

install: all
	install -Dm755 spotc $(PREFIX)/bin/spotc
	install -Dm755 spotc-fx $(PREFIX)/bin/spotc-fx
	install -Dm755 spotc-ipv4.so $(PREFIX)/bin/spotc-ipv4.so
	install -Dm755 spotc-reverse-cache $(PREFIX)/bin/spotc-reverse-cache

clean:
	rm -f spotc spotc-fx spotc-ipv4.so spotc-reverse-cache src/*.o

.PHONY: all install clean
