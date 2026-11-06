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

app: macbat              # MacBat.app, the macOS menu bar app, into dist/ (macOS 13+ SDK, Swift)
	mkdir -p dist && cp macbat dist/macbat-macos-universal && packaging/build-app.sh

release:                 # all assets into dist/ (macOS host with Docker; MACBAT_NOTARIZE=1 to notarize)
	packaging/release.sh

clean:
	rm -rf macbat dist

.PHONY: all install app release clean
