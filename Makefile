CXXFLAGS=-W -Wall -Wextra -O3
PREFIX=/usr/local

all: bidi-tee bidi-tee-print

bidi-tee:

install: bidi-tee
	install bidi-tee bidi-tee-print $(PREFIX)/bin

clean:
	rm -f bidi-tee bidi-tee-print
