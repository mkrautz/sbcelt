// Copyright (C) 2012 The SBCELT Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE-file.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>

#include <linux/prctl.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/prctl.h>

#include "celt.h"
#include "../sbcelt-internal.h"

#include "futex.h"
#include "seccomp-sandbox.h"

#define PAGE_SIZE   4096
#define SAMPLE_RATE 48000

#ifdef DEBUG
# define debugf(fmt, ...) \
	do { \
		fprintf(stderr, "sbcelt-helper:%s():%u: " fmt "\n", \
			__FILE__, __LINE__, ## __VA_ARGS__); \
		fflush(stderr); \
	} while (0)
#else
 #define debugf(s, ...) do{} while (0)
#endif

static struct SBCELTWorkPage *workpage = NULL;
static struct SBCELTDecoderPage *decpage = NULL;
static CELTMode *modes[SBCELT_SLOTS];
static CELTDecoder *decoders[SBCELT_SLOTS];

int SBCELT_FutexHelper() {
	while (1) {
		unsigned char *src = &workpage->encbuf[0];
		float *dst = &workpage->decbuf[0];

		// Wait for the lib to signal us.
		while (workpage->ready == 1) {
			int err = futex_wait(&workpage->ready, 1, NULL);
			if (err == 0 || err == EWOULDBLOCK) {
				break;
			}
		}

		debugf("waiting for work...");

		int idx = workpage->slot;
		struct SBCELTDecoderSlot *slot = &decpage->slots[idx];
		CELTMode *m = modes[idx];
		CELTDecoder *d = decoders[idx];
		if (slot->dispose && m != NULL && d != NULL) {
			debugf("disposed of mode & decoder for slot=%i", idx);
			celt_mode_destroy(m);
			celt_decoder_destroy(d);
			m = modes[idx] = celt_mode_create(SAMPLE_RATE, SAMPLE_RATE / 100, NULL);
			d = decoders[idx] = celt_decoder_create(m, 1, NULL);
			slot->dispose = 0;
		}
		if (m == NULL && d == NULL) {
			debugf("created mode & decoder for slot=%i", idx);
			m = modes[idx] = celt_mode_create(SAMPLE_RATE, SAMPLE_RATE / 100, NULL);
			d = decoders[idx] = celt_decoder_create(m, 1, NULL);
		}

		debugf("got work for slot=%i", idx);
		unsigned int len = workpage->len;
		debugf("to decode: %p, %p, %u, %p", d, src, len, dst);
		if (len == 0)
			celt_decode_float(d, NULL, 0, dst);
		else
			celt_decode_float(d, src, len, dst);

		debugf("decoded len=%u", len);

		workpage->ready = 1;

		if (!workpage->busywait)
			futex_wake(&workpage->ready);
	}

	return -2;
 }

int SBCELT_RWHelper() {
	while (1) {
		unsigned char *src = &workpage->encbuf[0];
		float *dst = &workpage->decbuf[0];

		// Wait for the lib to signal us.
		unsigned char _;
		if (read(0, &_, 1) == -1) {
			return -2;
		}

		debugf("waiting for work...");

		int idx = workpage->slot;
		struct SBCELTDecoderSlot *slot = &decpage->slots[idx];
		CELTMode *m = modes[idx];
		CELTDecoder *d = decoders[idx];
		if (slot->dispose && m != NULL && d != NULL) {
			debugf("disposed of mode & decoder for slot=%i", idx);
			celt_mode_destroy(m);
			celt_decoder_destroy(d);
			m = modes[idx] = celt_mode_create(SAMPLE_RATE, SAMPLE_RATE / 100, NULL);
			d = decoders[idx] = celt_decoder_create(m, 1, NULL);
			slot->dispose = 0;
		}
		if (m == NULL && d == NULL) {
			debugf("created mode & decoder for slot=%i", idx);
			m = modes[idx] = celt_mode_create(SAMPLE_RATE, SAMPLE_RATE / 100, NULL);
			d = decoders[idx] = celt_decoder_create(m, 1, NULL);
		}

		debugf("got work for slot=%i", idx);
		unsigned int len = workpage->len;
		debugf("to decode: %p, %p, %u, %p", d, src, len, dst);
		if (len == 0)
			celt_decode_float(d, NULL, 0, dst);
		else
			celt_decode_float(d, src, len, dst);

		debugf("decoded len=%u", len);

		if (write(1, dst, sizeof(float)*480) == -1) {
			return -3;
		}
	}
}

int main(int argc, char *argv[]) {
	if (argc >= 2 && !strcmp(argv[1], "seccomp-detect")) {
		debugf("in seccomp-detect mode");
		if (seccomp_sandbox_filter_init() == 0)
			_exit(SBCELT_SANDBOX_SECCOMP_BPF);
		if (seccomp_sandbox_strict_init() == 0)
			_exit(SBCELT_SANDBOX_SECCOMP_STRICT);
		_exit(SBCELT_SANDBOX_NONE);
	}

	debugf("helper running");

	if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1)
		return 1;

	char shmfn[50];
	if (snprintf(&shmfn[0], 50, "/sbcelt-%lu", (unsigned long) getppid()) < 0)
		return 2;

	int fd = shm_open(&shmfn[0], O_RDWR, 0600);
	if (fd == -1) {
		debugf("unable to open shm: %s (%i)", strerror(errno), errno);
		return 3;
	}

	void *addr = mmap(NULL, SBCELT_SLOTS*PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_FILE|MAP_SHARED, fd, 0);
	workpage = addr;
	decpage = addr+PAGE_SIZE;

	debugf("workpage=%p, decpage=%p", workpage, decpage);

	switch (workpage->sandbox) {
		case SBCELT_SANDBOX_NONE:
			// No sandboxing available.
			break;
		case SBCELT_SANDBOX_SECCOMP_STRICT: {
			if (seccomp_sandbox_strict_init() == -1) {
				debugf("unable to set up strict seccomp");
				return 4;
			}
			break;
		}
		case SBCELT_SANDBOX_SECCOMP_BPF: {
			if (seccomp_sandbox_filter_init() == -1) {
				debugf("unable to set up filtered seccomp");
				return 4;
			}
			break;
		}
	}

	switch (workpage->mode) {
		case SBCELT_MODE_FUTEX:
			return SBCELT_FutexHelper();
		case SBCELT_MODE_RW:
			return SBCELT_RWHelper();
	}

	return 5;
}
