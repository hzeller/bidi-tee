CXXFLAGS=-W -Wall -Wextra -pedantic -O3 -D_POSIX_C_SOURCE=199309L

PREFIX=/usr/local

all: bidi-tee bidi-tee-print

bidi-tee:

install: bidi-tee
	install bidi-tee bidi-tee-print $(PREFIX)/bin

clean:
	rm -f bidi-tee bidi-tee-print
