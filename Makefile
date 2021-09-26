CXXFLAGS=-W -Wall -Wextra -O3
PREFIX=/usr/local

bidi-tee:

install: bidi-tee
	install bidi-tee $(PREFIX)/bin

clean:
	rm -f bidi-tee
