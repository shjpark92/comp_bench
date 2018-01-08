CC=gcc

all:
	git submodule update --init --recursive
	cd ./brotli && ./configure && make lib
	cd ./zlib && ./configure && make
	$(CC) main.c -O3 -c -o main.o -I ./zlib -I ./brotli/c/include
	$(CC) main.o -lpthread ./zlib/libz.a ./brotli/libbrotli.a -lm -o bench
