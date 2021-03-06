#ifndef PENDULUM_H
#define PENDULUM_H

#define PENDULUM_VERSION  1

#include <stdint.h>

#define PN_HEAP_SIZE     10
#define PN_SLOT_CHUNK   256

typedef uint8_t  pn_byte;
typedef uint64_t pn_word;
typedef struct pn_machine pn_machine;
typedef pn_word (*pn_function)(pn_machine*);
typedef pn_word (*pn_fnpragma)(pn_machine*, const char*, const char*);

typedef struct { pn_word op, arg1, arg2; } pn_opcode;
typedef struct {
	char        name[32];
	pn_function call;
} pn_func_t;
typedef struct {
	char     label[64];
	pn_word  step;
} pn_jump_t;
typedef struct {
	char     label[64];
	pn_byte  value;
} pn_flag_t;

struct pn_machine {
	pn_word A,  B,  C,
	        D,  E,  F;
	pn_word S1, S2;
	pn_word T1, T2, Tr;
	pn_word R,  Er;
	pn_word Ip, Dp;
	const char *topic;
	uint32_t topics;

	void *heap[PN_HEAP_SIZE];

	void *U;
	pn_fnpragma pragma;

	FILE *dump_fd;
	int trace;

	size_t     datasize;
	pn_byte   *data;

	size_t     codesize;
	pn_opcode *code;

	size_t    nfuncs;
	pn_func_t *funcs;

	size_t    nflags;
	pn_flag_t *flags;

	size_t    njumps;
	pn_jump_t *jumps;
};


#define PN_ATTR_DUMPFD 0x0001

int pn_init(pn_machine *m);
int pn_destroy(pn_machine *m);

int pn_set(pn_machine *m, int attr, void *value);
int pn_flag(pn_machine *m, const char *label, int value);
int pn_flagged(pn_machine *m, const char *label);
int pn_func(pn_machine *m, const char *op, pn_function fn);
int pn_pragma(pn_machine *m, const char *name, const char *arg);
int pn_parse(pn_machine *m, FILE *io);
int pn_trace(pn_machine *m, const char *fmt, ...);
int pn_run(pn_machine *m);
int pn_run_safe(pn_machine *m);
int pn_die(pn_machine *m, const char *e);
int pn_heap_add(pn_machine *m, void *p);
int pn_heap_purge(pn_machine *m);

#endif
