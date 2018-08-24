CC=gcc

ifeq ($(shell uname), Darwin)
	argp:=-largp
endif

all:
	git submodule update --init --recursive
	cd ./brotli && ./configure && make lib -j
	cd ./zlib && ./configure && make -j
	cd ./zstd && make -j
	$(CC) main.c -O3 -c -o main.o -I ./zlib -I ./brotli/c/include -I ./zstd/lib
	$(CC) main.o -lpthread ./zlib/libz.a ./brotli/libbrotli.a ./zstd/lib/libzstd.a -lm ${argp} -o bench
