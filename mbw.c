/*
 * vim: ai ts=4 sts=4 sw=4 cinoptions=>4 expandtab
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "util.h"

/* how many runs to average by default */
#define DEFAULT_NR_LOOPS 10

/* we have 3 tests at the moment */
#define MAX_TESTS 4

/* default block size for test 2, in uint64_t */
#define DEFAULT_BLOCK_SIZE 262144


/* version number */
#define VERSION "1.4"

typedef struct
{
    const char *description;
    int use_tmpbuf;
    void (*f)(int64_t *, int64_t *, int);
} bench_info;
/*
 * MBW memory bandwidth benchmark
 *
 * 2006, 2012 Andras.Horvath@gmail.com
 * 2013 j.m.slocum@gmail.com
 * (Special thanks to Stephen Pasich)
 *
 * http://github.com/raas/mbw
 *
 * compile with:
 *			gcc -O -o mbw mbw.c
 *
 * run with eg.:
 *
 *			./mbw 300
 *
 * or './mbw -h' for help
 *
 * watch out for swap usage (or turn off swap)
 */

void usage()
{
    printf("mbw memory benchmark v%s, https://github.com/raas/mbw\n", VERSION);
    printf("Usage: mbw [options] array_size_in_MiB\n");
    printf("Options:\n");
    printf("	-n: number of runs per test (0 to run forever)\n");
    printf("	-a: Don't display average\n");
    printf("	-b <size>: block size in bytes for -t2 (default: %d)\n", DEFAULT_BLOCK_SIZE);
    printf("	-q: quiet (print statistics only)\n");
    printf("(will then use two arrays, watch out for swapping)\n");
    printf("'Bandwidth' is amount of data copied over the time this operation took.\n");
    printf("\nThe default is to run all tests available.\n");
}

void* mempcpy(void* dst, const void* src, size_t n) {
  return memcpy(dst, src, n) + n;
}

/* ------------------------------------------------------ */

/* allocate a test array and fill it with data
 * so as to force Linux to _really_ allocate it */
int64_t *make_array(int64_t asize)
{
    int64_t t;
    unsigned int long_size=sizeof(int64_t);
    int64_t *a;

    a=calloc(asize, long_size);

    if(NULL==a) {
        perror("Error allocating memory");
        exit(1);
    }

    /* make sure both arrays are allocated, fill with pattern */
    for(t=0; t<asize; t++) {
        a[t]=0xaa;
    }
    return a;
}
void memcpy_wrapper(int64_t *dst, int64_t *src, int size)
{
    memcpy(dst, src, size);
}

static bench_info c_benchmarks[] =
{
    { "C copy backwards", 0, aligned_block_copy_backwards },
    { "C copy backwards (32 byte blocks)", 0, aligned_block_copy_backwards_bs32 },
    { "C copy backwards (64 byte blocks)", 0, aligned_block_copy_backwards_bs64 },
    { "C copy", 0, aligned_block_copy },
    { "C copy prefetched (32 bytes step)", 0, aligned_block_copy_pf32 },
    { "C copy prefetched (64 bytes step)", 0, aligned_block_copy_pf64 },
    { "C 2-pass copy", 1, aligned_block_copy },
    { "C 2-pass copy prefetched (32 bytes step)", 1, aligned_block_copy_pf32 },
    { "C 2-pass copy prefetched (64 bytes step)", 1, aligned_block_copy_pf64 },
    { "C fill", 0, aligned_block_fill },
    { "C fill (shuffle within 16 byte blocks)", 0, aligned_block_fill_shuffle16 },
    { "C fill (shuffle within 32 byte blocks)", 0, aligned_block_fill_shuffle32 },
    { "C fill (shuffle within 64 byte blocks)", 0, aligned_block_fill_shuffle64 },
    { "standard memcpy ", 0, memcpy_wrapper },
    { "fill write ", 0, fill_write }
};
/* actual benchmark */
/* asize: number of type 'int64_t' elements in test arrays
 * long_size: sizeof(int64_t) cached
 * type: 0=use memcpy, 1=use dumb copy loop (whatever GCC thinks best)
 *
 * return value: elapsed time in seconds
 */
double worker(int64_t asize, int64_t *a, int64_t *b, int64_t *c,  int cached, int64_t block_size, int use_tmpbuf,  void (*f)(int64_t *, int64_t *, int))
{
    int64_t t;
    struct timeval starttime, endtime;
    double te;
    unsigned int long_size=sizeof(int64_t);
    /* array size in bytes */
    int64_t array_bytes=asize*long_size;

    char* aa = (char*)a;
    char* bb = (char*)b;
    gettimeofday(&starttime, NULL);
    if(cached){
        for (t=array_bytes; t >= block_size; t-=block_size, aa+=block_size){
            bb=mempcpy(bb, aa, block_size);
        }
        if(t) {
            bb=mempcpy(bb, aa, t);
        }

    }
    else if(use_tmpbuf){
        for(t = 0; t < array_bytes ; t += block_size){
            f(c , a + t/long_size, block_size);
            f(b + t/long_size , c, block_size);
        }
    }
    else{
        f(b, a, array_bytes);
    }
    gettimeofday(&endtime, NULL);

    te=((double)(endtime.tv_sec*1000000-starttime.tv_sec*1000000+endtime.tv_usec-starttime.tv_usec))/1000000;

    return te;
}

/* ------------------------------------------------------ */

/* pretty print worker's output in human-readable terms */
/* te: elapsed time in seconds
 * mt: amount of transferred data in MiB
 * type: see 'worker' above
 *
 * return value: -
 */
void printout(double te, double mt, int type, const char *description)
{
    printf("%-52s", description);
    printf("Elapsed: %.5f\t", te);
    printf("MiB: %.5f\t", mt);
    printf("Copy: %.3f MiB/s\n", mt/te);
    return;
}

/* ------------------------------------------------------ */

int main(int argc, char **argv)
{
    unsigned int long_size=0;
    double te, te_sum; /* time elapsed */
    int64_t asize=0; /* array size (elements in array) */
    int i;
    int64_t *a, *b, *c; /* the two arrays to be copied from/to */
    int o; /* getopt options */
    int64_t testno = 0;

    /* options */

    /* how many runs to average? */
    int nr_loops=DEFAULT_NR_LOOPS;
    /* fixed memcpy block size for -t2 */
    int64_t block_size=DEFAULT_BLOCK_SIZE;
    /* show average, -a */
    int showavg=1;
    /* what tests to run (-t x) */
    int runid=-1;
    double mt=0; /* MiBytes transferred == array size in MiB */
    int quiet=0; /* suppress extra messages */
    int cached = 0;

    while((o=getopt(argc, argv, "chaqn:t:b:")) != EOF) {
        switch(o) {
            case 'h':
                usage();
                exit(1);
                break;
            case 'a': /* suppress printing average */
                showavg=0;
                break;
            case 'c': /* suppress printing average */
                cached=1;
                break;
            case 'n': /* no. loops */
                nr_loops=strtoul(optarg, (char **)NULL, 10);
                break;
            case 't': /* test to run */
                runid=strtoul(optarg, (char **)NULL, 10);
                if(0>runid) {
                    printf("Error: test number must be between 0 and %d\n", MAX_TESTS);
                    exit(1);
                }
                break;
            case 'b': /* block size in int64*/
                block_size=strtoull(optarg, (char **)NULL, 10);
                if(0>=block_size) {
                    printf("Error: what block size do you mean?\n");
                    exit(1);
                }
                break;
            case 'q': /* quiet */
                quiet=1;
                break;
            default:
                break;
        }
    }

    if(optind<argc) {
        mt=strtoul(argv[optind++], (char **)NULL, 10);
    } else {
        printf("Error: no array size given!\n");
        exit(1);
    }

    if(0>=mt) {
        printf("Error: array size wrong!\n");
        exit(1);
    }

    /* ------------------------------------------------------ */

    long_size=sizeof(int64_t); /* the size of int64_t on this platform */
    asize=1024*1024/long_size*mt; /* how many longs then in one array? */

    if(asize*long_size < block_size) {
        printf("Error: array size larger than block size (%llu bytes)!\n", block_size);
        exit(1);
    }

    if(!quiet) {
        printf("int64_t uses %d bytes. ", long_size);
        printf("Allocating 2*%lld elements = %lld bytes of memory.\n", asize, 2*asize*long_size);
        printf("Using %lld bytes as blocks for memcpy block copy test.\n", block_size);
    }

    a=make_array(asize);
    b=make_array(asize);
    c=make_array(block_size);

    /* ------------------------------------------------------ */
    if(!quiet) {
        printf("Getting down to business... Doing %d runs per test.\n", nr_loops);
    }

    printf("runid :%d\n", runid);
    /* run all tests requested, the proper number of times */
    for(testno=0; testno<sizeof(c_benchmarks)/sizeof(bench_info); testno++) {
        te_sum=0;
        if(runid < 0 || (runid == testno)) {
            for (i=0; nr_loops==0 || i<nr_loops; i++) {
                te=worker(asize, a, b, c, cached, block_size, c_benchmarks[testno].use_tmpbuf, c_benchmarks[testno].f);
                te_sum+=te;
                /* printf("%d\t", i); */
                /* printout(te, mt, testno, c_benchmarks[testno].description); */
            }
            if(showavg) {
                printf("AVG\t");
                printout(te_sum/nr_loops, mt, testno, c_benchmarks[testno].description);
            }
        }
    }

    free(a);
    free(b);
    free(c);
    return 0;
}

