#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>

#include <celt.h>
#include "common.h"

#define USEC_PER_SEC  1000000
#define NSEC_PER_USEC 1000

// Monotonic microsecond timestamp generator.
// The expectation is that gettimeofday() uses
// the VDSO and/or the TSC (on modern x86) to
// avoid system calls.
uint64_t mtime() {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == -1) {
                return 0;
        }
        return ((uint64_t)tv.tv_sec * USEC_PER_SEC) + (uint64_t)tv.tv_usec;
}

int gentest(unsigned char *buf, int n); // gentest.c

#define GEN_TESTDATA_FILE 0

int main(int argc, char *argv[]) {
	int i;
	int niter = 10000;

	unsigned char buf[127];
#if GEN_TESTDATA_FILE == 1
	if (gentest(buf, 127) == -1) {
		fprintf(stderr, "unable to generate testdata\n");
		return 1;
	}

	FILE *f = fopen("test.dat", "w");
	fwrite(buf, 127, 1, f);
	fclose(f);
#else
	FILE *f = fopen("test.dat", "r");
	if (f == NULL) {
		fprintf(stderr, "unable to open test.dat\n");
              return 1;
        }
        if (fread(buf, 1, 127, f) != 127) {
                fprintf(stderr, "unable to read test.dat\n");
                return 1;
        }
        fclose(f);
#endif

	CELTMode *dm = celt_mode_create(SAMPLE_RATE, SAMPLE_RATE / 100, NULL);
        CELTDecoder *d = celt_decoder_create(dm, 1, NULL);

	float pcmout[FRAME_SIZE];
	uint64_t begin = mtime();
	for (i = 0; i < niter; i++) {
		if (celt_decode_float(d, buf, 127, pcmout) != CELT_OK) {
			fprintf(stderr, "unable to decode...\n");
			return 1;
		}
	}
	uint64_t elapsed = mtime() - begin;

	printf("{\"niter\": %i, \"elapsed_usec\": %lu}\n", niter, elapsed);

	return 0;
}
