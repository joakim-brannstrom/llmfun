CC?=	/usr/bin/cc

all: build/linenoise.o

build/linenoise.o: vendor/linenoise/linenoise.c vendor/linenoise/linenoise.h
	mkdir -p build
	$(CC) -c -O2 vendor/linenoise/linenoise.c -o build/linenoise.o

clean:
	rm -f build/linenoise.o
