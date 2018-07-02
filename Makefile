CC=gcc

ifeq ($(shell uname), Darwin)
	argp:=-largp
endif

all:
	git submodule update --init --recursive
	cd ./brotli && ./configure && make lib -j
	cd ./zlib && ./configure && make -j
	$(CC) main.c -O3 -c -o main.o -I ./zlib -I ./brotli/c/include
	$(CC) main.o -lpthread ./zlib/libz.a ./brotli/libbrotli.a -lm ${argp} -o bench
