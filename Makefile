VERSION := $(shell sed -n 's/^\#define MACBAT_VERSION "\(.*\)"/\1/p' macbat.c)
CFLAGS  ?= -O2 -Wall -Wextra
UNAME   := $(shell uname -s)

all: macbat

ifeq ($(UNAME),Darwin)
macbat: macbat.c
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -o $@ $< -framework IOKit -framework CoreFoundation
	codesign -s - -f $@
else
macbat: macbat.c
	$(CC) $(CFLAGS) -o $@ $<
endif

install: macbat          # sudo make install
	./macbat install

release:                 # all assets into dist/ (macOS host with Docker)
	packaging/release.sh

clean:
	rm -rf macbat dist

.PHONY: all install release clean
