/* Copyright (c) 2017, Cloudflare, Inc. */

#include <argp.h>
#include <fcntl.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "brotli/encode.h"
#include "zlib.h"
#include "zstd.h"

typedef struct {
  uint8_t *data;
  size_t len;
  uint8_t *dict;
  size_t dict_len;
  uint64_t ctr;
  double start_ns;
  size_t out_size;
  int level;
  int brotli : 1;
  int zstd: 1;
  int dec : 1;
} compress_task;

static void gzip_bench(compress_task *task) {
  z_stream strm = {0};
  uint8_t null_buf[32768];

  deflateInit2(&strm, task->level, Z_DEFLATED, 15 /*+ 16*/, 8, Z_DEFAULT_STRATEGY);

  if (task->dict) {
    deflateSetDictionary(&strm, task->dict, 32768 < task->dict_len ? 32768 : task->dict_len);
  }
  strm.next_in = task->data;
  strm.avail_in = task->len;

  // The benchmark discards the output
  strm.next_out = null_buf;
  strm.avail_out = sizeof(null_buf);

  while (strm.avail_in > 0) {
    deflate(&strm, 0);
    strm.next_out = null_buf;
    strm.avail_out = sizeof(null_buf);
  }

  while (deflate(&strm, Z_FINISH) != Z_STREAM_END) {
    strm.next_out = null_buf;
    strm.avail_out = sizeof(null_buf);
  }

  task->out_size = strm.total_out;
  deflateEnd(&strm);
}

static void br_bench(compress_task *task) {
  BrotliEncoderState *strm = BrotliEncoderCreateInstance(NULL, NULL, NULL);
  BrotliEncoderSetParameter(strm, BROTLI_PARAM_QUALITY, task->level);

  if (task->dict) {
    BrotliEncoderSetCustomDictionary(strm, task->dict_len, task->dict);
  }

  size_t available_in;
  const uint8_t *next_in;
  size_t available_out;
  uint8_t *next_out;
  size_t total_out;
  uint8_t null_buf[32768];

  next_in = task->data;
  available_in = task->len;

  next_out = null_buf;
  available_out = sizeof(null_buf);

  while (available_in > 0) {
    BrotliEncoderCompressStream(strm, BROTLI_OPERATION_PROCESS, &available_in,
                                &next_in, &available_out, &next_out,
                                &total_out);
    available_out = sizeof(null_buf);
    next_out = null_buf;
  }

  while (!BrotliEncoderIsFinished(strm)) {
    BrotliEncoderCompressStream(strm, BROTLI_OPERATION_FINISH, &available_in,
                                &next_in, &available_out, &next_out,
                                &total_out);
    available_out = sizeof(null_buf);
    next_out = null_buf;
  }

  task->out_size = total_out;
  BrotliEncoderDestroyInstance(strm);
}

static void zstd_bench(ZSTD_CStream *strm, compress_task *task) {

  size_t total_out = 0;
  uint8_t null_buf[32768];

  ZSTD_resetCStream(strm, ZSTD_CONTENTSIZE_UNKNOWN);
  ZSTD_inBuffer in = (ZSTD_inBuffer){.src = task->data, .size = task->len, .pos = 0};
  ZSTD_outBuffer out = (ZSTD_outBuffer){.dst = null_buf, .size = sizeof(null_buf), .pos = 0};

  while (in.pos < in.size) {
    ZSTD_compressStream(strm, &out, &in);
    total_out += out.pos;
    out = (ZSTD_outBuffer){.dst = null_buf, .size = sizeof(null_buf), .pos = 0};
  }

  while (ZSTD_endStream(strm, &out)) {
    total_out += out.pos;
    out = (ZSTD_outBuffer){.dst = null_buf, .size = sizeof(null_buf), .pos = 0};
  }

  total_out += out.pos;

  task->out_size = total_out;
}

void *wrapper(void *arg) {
  compress_task *task = (compress_task *)arg;
  struct timespec cur_time;
  double start_time_ns = task->start_ns;
  uint64_t ctr = 0;

  ZSTD_CStream *strm = NULL;

  if (task->zstd) {
    strm = ZSTD_createCStream();
    if (task->dict) {
      ZSTD_CDict *dict = ZSTD_createCDict(task->dict, task->dict_len, task->level);
      ZSTD_initCStream_usingCDict(strm, dict);
      ZSTD_CCtx_setParameter(strm, 100 /* ZSTD_p_compressionLevel */, task->level);
    } else {
      ZSTD_initCStream(strm, task->level);
    }
  }

  do {
    clock_gettime(CLOCK_MONOTONIC, &cur_time);

    double cur_time_ns = cur_time.tv_sec * 1e9 + cur_time.tv_nsec;
    if (start_time_ns + 10 * 1e9 < cur_time_ns) {
      break;
    }

    if (task->brotli) {
      br_bench(task);
    } else if (task->zstd) {
      zstd_bench(strm, task);
    } else {
      gzip_bench(task);
    }
    ctr++;

  } while (1);

  ZSTD_freeCStream(strm);
  task->ctr = ctr;

  return NULL;
}

typedef struct {
  int c, q, d, b, s;
  char *file_name;
  char *dict_name;
} cmd_line_options;

const char *argp_program_version = "comp bench 0.0001-alpha";
const char *argp_program_bug_address = "</dev/null>";

/* Program documentation. */
static char doc[] = "Runs gzip or brotli on multiple threads";

/* A description of the arguments we accept. */
static char args_doc[] = "file1";

static struct argp_option options[] = {
    {"concurency", 'c', "concurency", 0, "Number of threads"},
    {"quality", 'q', "quality", 0, "Quality level"},
    {"brotli", 'b', 0, 0, "Benchmark brotli"},
    {"zstd", 's', 0, 0, "Benchmark Zstd"},
    {"dict", 'd', "dictionary", 0, "Use dictionary"},
    {0}};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
  cmd_line_options *arguments = state->input;
  switch (key) {
  case 'c':
    arguments->c = atoi(arg);
    break;
  case 'q':
    arguments->q = atoi(arg);
    break;
  case 'b':
    arguments->b = 1;
    break;
  case 's':
    arguments->s = 1;
    break;
  case 'd':
    arguments->dict_name = arg;
    break;

  case ARGP_KEY_ARG:
    if (state->arg_num >= 1) {
      argp_usage(state);
    }

    arguments->file_name = arg;
    break;

  case ARGP_KEY_END:
    if (state->arg_num < 1) {
      argp_usage(state);
    }
    break;

  default:
    return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

/* Our argp parser. */
static struct argp argp = {options, parse_opt, args_doc, doc};

int main(int argc, char *argv[]) {
  pthread_t *threads;
  compress_task *tasks;
  int i;
  uint8_t *buf = NULL, *dict = NULL;
  size_t len, dict_len;
  struct stat s;
  uint64_t total = 0;
  struct timespec start_time;

  cmd_line_options arguments =
      (cmd_line_options){.file_name = "", .q = 8, .c = 1};
  argp_parse(&argp, argc, argv, 0, 0, &arguments);

  int fd = open(arguments.file_name, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "Error opening %s\n", arguments.file_name);
    return -1;
  }
  fstat(fd, &s);
  len = s.st_size;

  int dict_fd = 0;
  if (arguments.dict_name) {
    dict_fd = open(arguments.dict_name, O_RDONLY);
    if (dict_fd < 0) {
      fprintf(stderr, "Error opening %s\n", arguments.dict_name);
      return -1;
    }
    fstat(dict_fd, &s);
    dict_len = s.st_size;
  }

  char *alg = "gzip";
  if (arguments.b) {
    alg = "brotli";
  } else if (arguments.s) {
    alg = "Zstd";
  }

  fprintf(stderr, "Tested file %s; size: %zu\n", arguments.file_name, len);
  fprintf(stderr, "Threads: %d, alg: %s, quality %d\n", arguments.c, alg, arguments.q);

  buf = mmap(0, len, PROT_READ, MAP_PRIVATE, fd, 0);
  if (dict_fd > 0) {
    dict = mmap(0, dict_len, PROT_READ, MAP_PRIVATE, dict_fd, 0);
  }

  int nthreads = arguments.c;
  threads = malloc(nthreads * sizeof(pthread_t));
  tasks = malloc(nthreads * sizeof(compress_task));

  clock_gettime(CLOCK_MONOTONIC, &start_time);
  double start_time_ns = start_time.tv_sec * 1e9 + start_time.tv_nsec;

  for (i = 0; i < nthreads; i++) {
    tasks[i] = (compress_task){.data = buf,
                               .len = len,
			       .dict = dict,
			       .dict_len = dict_len,
                               .brotli = arguments.b,
                               .zstd = arguments.s,
                               .level = arguments.q,
                               .start_ns = start_time_ns};
    pthread_create(&threads[i], NULL, wrapper, &tasks[i]);
  }

  for (i = 0; i < nthreads; i++) {
    pthread_join(threads[i], NULL);
    total += tasks[i].ctr;
  }

  fprintf(stderr, "Total times compressed: %"PRId64"; compressed size: %ld\n", total,
          tasks[0].out_size);
  fprintf(stdout, "Compression speed:,%0.2f,MiB\n",
          (double)total * len / 1024 / 1024 / 10);
}
