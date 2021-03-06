#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pendulum.h"
#include "cw.h"

#define  PN_OP_NOOP     0x0000
#define  PN_OP_OK       0x0001
#define  PN_OP_NOTOK    0x0002
#define  PN_OP_EQ       0x0003
#define  PN_OP_NE       0x0004
#define  PN_OP_CMP      0x0005
#define  PN_OP_GT       0x0006
#define  PN_OP_GTE      0x0007
#define  PN_OP_LT       0x0008
#define  PN_OP_LTE      0x0009
#define  PN_OP_COPY     0x000a
#define  PN_OP_SET      0x000b
#define  PN_OP_HALT     0x000c
#define  PN_OP_ERROR    0x000d
#define  PN_OP_LOG      0x000e
#define  PN_OP_CALL     0x000f
#define  PN_OP_PRINT    0x0010
#define  PN_OP_JUMP     0x0011
#define  PN_OP_DUMP     0x0012
#define  PN_OP_TRACE    0x0013
#define  PN_OP_VCHECK   0x0014
#define  PN_OP_SYSERR   0x0015
#define  PN_OP_FLAG     0x0016
#define  PN_OP_FLAGGED  0x0017
#define  PN_OP_DIFF     0x0018
#define  PN_OP_PRAGMA   0x0019
#define  PN_OP_NFLAGGED 0x001a
#define  PN_OP_RESET    0x001b
#define  PN_OP_TOPIC    0x001c
#define  PN_OP_GETENV   0x001d
#define  PN_OP_SETENV   0x001e
#define  PN_OP_INVAL    0x00ff

#define PC      pn_CODE(m, m->Ip)
#define TRACE_START "TRACE :: %08lx [%02x] %s"
#define TRACE_ARGS  (unsigned long)m->Ip, (unsigned int)PC.op, OP_NAMES[PC.op]

static const char *OP_NAMES[] = {
	"NOOP",
	"OK?",
	"NOTOK?",
	"EQ?",
	"NE?",
	"CMP?",
	"GT?",
	"GTE?",
	"LT?",
	"LTE?",
	"COPY",
	"SET",
	"HALT",
	"ERROR",
	"LOG",
	"CALL",
	"PRINT",
	"JUMP",
	"DUMP",
	"TRACE",
	"VCHECK",
	"SYSERR",
	"FLAG",
	"FLAGGED?",
	"DIFF?",
	"PRAGMA",
	"!FLAGGED?",
	"RESET",
	"TOPIC",
	"GETENV",
	"SETENV",
	"(invalid)",
	NULL,
};

static const char *REG_NAMES[] = {
	 "A",  "B",  "C",
	 "D",  "E",  "F",
	"S1", "S2",
	"T1", "T2", "Tr",
	 "R", "Er",
	"Ip", "Dp",
	NULL,
};

#define pn_REG(m,off) ((pn_byte*)m + offsetof(pn_machine, A) + off * sizeof(pn_word))
#define pn_DATA(m,off) ((m)->data[off])
#define pn_ADDR(m,off) ((m)->data+(off))
#define pn_CODE(m,off) ((m)->code[off])

/* tagged values

   The most-significant (left-most) 4 bits
   identify the type of value.  Remaining
   bits encode the value.
 */
#define     TAG_MASK 0xf000000000000000UL
#define   VALUE_MASK 0x0fffffffffffffffUL

#define REGISTER_TAG 0x9000000000000000UL /* 1001 */
#define    LABEL_TAG 0xa000000000000000UL /* 1010 */
#define     FLAG_TAG 0xb000000000000000UL /* 1011 */
#define FUNCTION_TAG 0xc000000000000000UL /* 1100 */

#define IS_REGISTER(x) (((x) & TAG_MASK) == REGISTER_TAG)
#define IS_LABEL(x)    (((x) & TAG_MASK) == LABEL_TAG)
#define IS_FLAG(x)     (((x) & TAG_MASK) == FLAG_TAG)
#define IS_FUNCTION(x) (((x) & TAG_MASK) == FUNCTION_TAG)

#define TAGV(x) ((x) & VALUE_MASK)

#define REGISTER(x) ((x) | REGISTER_TAG)
#define LABEL(x)    ((x) | LABEL_TAG)
#define FLAG(x)     ((x) | FLAG_TAG)
#define FUNCTION(x) ((x) | FUNCTION_TAG)

static int s_next_free_jump_slot(pn_machine *m)
{
	int i;
	for (i = 0; i < m->njumps; i++) {
		if (*m->jumps[i].label) continue;
		return i;
	}
	size_t new_size = m->njumps += PN_SLOT_CHUNK;
	pn_jump_t *x = realloc(m->jumps, sizeof(pn_jump_t) * new_size);
	if (!x) {
		errno = ENOBUFS;
		return -1;
	}

	memset(x + i, 0, sizeof(pn_jump_t) * (new_size - i));
	m->jumps = x;
	m->njumps = new_size;
	return i;
}

static int s_next_free_flag_slot(pn_machine *m)
{
	int i;
	for (i = 0; i < m->nflags; i++) {
		if (*m->flags[i].label) continue;
		return i;
	}
	size_t new_size = m->nflags += PN_SLOT_CHUNK;
	pn_flag_t *x = realloc(m->flags, sizeof(pn_flag_t) * new_size);
	if (!x) {
		errno = ENOBUFS;
		return -1;
	}

	memset(x + i, 0, sizeof(pn_flag_t) * (new_size - i));
	m->flags = x;
	m->nflags = new_size;
	return i;
}

static int s_next_free_func_slot(pn_machine *m)
{
	int i;
	for (i = 0; i < m->nfuncs; i++) {
		if (*m->funcs[i].name) continue;
		return i;
	}
	size_t new_size = m->nfuncs += PN_SLOT_CHUNK;
	pn_func_t *x = realloc(m->funcs, sizeof(pn_func_t) * new_size);
	if (!x) {
		errno = ENOBUFS;
		return -1;
	}

	memset(x + i, 0, sizeof(pn_func_t) * (new_size - i));
	m->funcs = x;
	m->nfuncs = new_size;
	return i;
}

static int s_label(char *buf, char **labelv)
{
	assert(buf);
	char *s, *p;

	s = strchr(buf, ';');
	if (s) *s = '\0';

	for (s = buf; *s && isspace(*s); s++);
	for (p = s; *s && *s != ':' && !isspace(*s); s++);
	if (*s != ':') return -1;
	*s = '\0';

	if (labelv) *labelv = strdup(p);
	return 0;
}

static int s_parse(char *buf, char **opv, char **arg1v, char **arg2v)
{
	char *s, *op, **arg, *arg1, *arg2, *p;
	int n = 1;

	s = strchr(buf, ';');
	if (s) *s = '\0';

	/* delimit the opcode mnemonic */
	for (s = buf; *s && isspace(*s); s++);
	for (op = s; *s && !isspace(*s); s++);
	if (*s) *s++ = '\0';
	if (!*op) return -1;

	arg = &arg1;
argument:
	p = s;
	for (; *s && isspace(*s); s++);
	*arg = p;
	if (*s == '"') {
		/* we keep the leading '"', to identify strings */
		for (*p++ = '"', s++; *s && *s != '"'; *p++ = *s++) {
			if (*s == '\\') {
				switch (*(++s)) {
					case 'n': *s = '\n'; break;
					case 'r': *s = '\r'; break;
					case 't': *s = '\t'; break;
					default: break;
				}
			}
		}
	} else {
		for (; *s && !isspace(*s); *p++ = *s++);
	}
	if (s == p && *s) s++;
	*p++ = '\0';

	arg = &arg2;
	if (n--) goto argument;

	if (opv)     *opv = strdup(op);
	if (arg1v) *arg1v = strdup(arg1);
	if (arg2v) *arg2v = strdup(arg2);
	return 0;
}

static int s_resolve_op(const char *op)
{
	if (strcmp(op, "NOOP")      == 0) return PN_OP_NOOP;
	if (strcmp(op, "PRAGMA")    == 0) return PN_OP_PRAGMA;
	if (strcmp(op, "EQ?")       == 0) return PN_OP_EQ;
	if (strcmp(op, "NE?")       == 0) return PN_OP_NE;
	if (strcmp(op, "CMP?")      == 0) return PN_OP_CMP;
	if (strcmp(op, "DIFF?")     == 0) return PN_OP_DIFF;
	if (strcmp(op, "GT?")       == 0) return PN_OP_GT;
	if (strcmp(op, "GTE?")      == 0) return PN_OP_GTE;
	if (strcmp(op, "LT?")       == 0) return PN_OP_LT;
	if (strcmp(op, "LTE?")      == 0) return PN_OP_LTE;
	if (strcmp(op, "OK?")       == 0) return PN_OP_OK;
	if (strcmp(op, "NOTOK?")    == 0) return PN_OP_NOTOK;
	if (strcmp(op, "COPY")      == 0) return PN_OP_COPY;
	if (strcmp(op, "SET")       == 0) return PN_OP_SET;
	if (strcmp(op, "HALT")      == 0) return PN_OP_HALT;
	if (strcmp(op, "ERROR")     == 0) return PN_OP_ERROR;
	if (strcmp(op, "LOG")       == 0) return PN_OP_LOG;
	if (strcmp(op, "CALL")      == 0) return PN_OP_CALL;
	if (strcmp(op, "PRINT")     == 0) return PN_OP_PRINT;
	if (strcmp(op, "JUMP")      == 0) return PN_OP_JUMP;
	if (strcmp(op, "DUMP")      == 0) return PN_OP_DUMP;
	if (strcmp(op, "TRACE")     == 0) return PN_OP_TRACE;
	if (strcmp(op, "VCHECK")    == 0) return PN_OP_VCHECK;
	if (strcmp(op, "SYSERR")    == 0) return PN_OP_SYSERR;
	if (strcmp(op, "FLAG")      == 0) return PN_OP_FLAG;
	if (strcmp(op, "FLAGGED?")  == 0) return PN_OP_FLAGGED;
	if (strcmp(op, "!FLAGGED?") == 0) return PN_OP_NFLAGGED;
	if (strcmp(op, "RESET")     == 0) return PN_OP_RESET;
	if (strcmp(op, "TOPIC")     == 0) return PN_OP_TOPIC;
	if (strcmp(op, "GETENV")    == 0) return PN_OP_GETENV;
	if (strcmp(op, "SETENV")    == 0) return PN_OP_SETENV;
	fprintf(stderr, "Invalid op: '%s'\n", op);
	return PN_OP_INVAL;
}

static pn_word s_resolve_reg(const char *reg)
{
#define reg_offset(M) REGISTER((offsetof(pn_machine, M) - offsetof(pn_machine, A))/sizeof(pn_word))
	if (strcmp(reg, "\%A")  == 0) return reg_offset(A);
	if (strcmp(reg, "\%B")  == 0) return reg_offset(B);
	if (strcmp(reg, "\%C")  == 0) return reg_offset(C);
	if (strcmp(reg, "\%D")  == 0) return reg_offset(D);
	if (strcmp(reg, "\%E")  == 0) return reg_offset(E);
	if (strcmp(reg, "\%F")  == 0) return reg_offset(F);

	if (strcmp(reg, "\%S1") == 0) return reg_offset(S1);
	if (strcmp(reg, "\%S2") == 0) return reg_offset(S2);

	if (strcmp(reg, "\%T1") == 0) return reg_offset(T1);
	if (strcmp(reg, "\%T2") == 0) return reg_offset(T2);
	if (strcmp(reg, "\%Tr") == 0) return reg_offset(Tr);

	if (strcmp(reg, "\%R")  == 0) return reg_offset(R);
	if (strcmp(reg, "\%Er") == 0) return reg_offset(Er);

	if (strcmp(reg, "\%Ip") == 0) return reg_offset(Ip);
	if (strcmp(reg, "\%Dp") == 0) return reg_offset(Dp);

	return -1;
#undef reg_offset
}

static pn_word s_resolve_arg(pn_machine *m, const char *arg)
{
	int i;
	pn_word val = 0;
	char ebuf[1024];

	switch (*arg) {
	case '"':
		strcpy((char *)pn_ADDR(m, m->Dp), arg+1);
		val = (pn_word)pn_ADDR(m, m->Dp);
		m->Dp += strlen(arg);
		return val;

	case '$':
		strcpy((char *)pn_ADDR(m, m->Dp), arg+1);
		val = (pn_word)pn_ADDR(m, m->Dp);
		m->Dp += strlen(arg);
		return val;

	case '%':
		return s_resolve_reg(arg);

	case '@':
		errno = ENOENT;
		for (i = 0; i < m->njumps; i++) {
			if (!*m->jumps[i].label) break;
			if (strcmp(m->jumps[i].label, arg+1) != 0)
				continue;

			errno = 0;
			cw_log(LOG_DEBUG, "Resolved label '%s' to instruction %08x",
				arg+1, m->jumps[i].step);
			return LABEL(m->jumps[i].step);
		}
		snprintf(ebuf, 1024, "Label '%s' not found\n", arg+1);
		pn_die(m, ebuf);

	case ':':
		for (i = 0; i < m->nflags; i++) {
			if (!m->flags[i].label[0])
				continue;
			if (strcmp(m->flags[i].label, arg+1) != 0)
				continue;

			cw_log(LOG_DEBUG, "Resolved flag '%s' to index %u (value=%i)",
				arg, i, m->flags[i].value);
			return FLAG((pn_word)i);
		}

		i = s_next_free_flag_slot(m);
		if (i < 0) {
			snprintf(ebuf, 1024, "Cannot set flag '%s': insufficient space\n", arg+1);
			pn_die(m, ebuf);
		}
		strncpy(m->flags[i].label, arg+1, 63);
		cw_log(LOG_DEBUG, "Flag '%s' is new, allocating space at index %u",
			arg, i);
		return FLAG((pn_word)i);

	case '&':
		errno = ENOENT;
		for (i = 0; i < m->nfuncs; i++) {
			if (!m->funcs[i].call) break;
			if (strcmp(m->funcs[i].name, arg+1)) continue;

			errno = 0;
			cw_log(LOG_DEBUG, "Resolved function '%s' to index %u / %p",
				arg+1, i, m->funcs[i].call);
			return FUNCTION((pn_word)i);
		}
		snprintf(ebuf, 1024, "'%s' is not a defined function\n", arg+1);
		pn_die(m, ebuf);

	default:
		if (strcmp(arg, "EMERG")   == 0) return LOG_EMERG;
		if (strcmp(arg, "ALERT")   == 0) return LOG_ALERT;
		if (strcmp(arg, "CRIT")    == 0) return LOG_CRIT;
		if (strcmp(arg, "ERR")     == 0) return LOG_ERR;
		if (strcmp(arg, "WARNING") == 0) return LOG_WARNING;
		if (strcmp(arg, "NOTICE")  == 0) return LOG_NOTICE;
		if (strcmp(arg, "INFO")    == 0) return LOG_INFO;
		if (strcmp(arg, "DEBUG")   == 0) return LOG_DEBUG;
		return strtoll(arg, NULL, 0);
	}
}

static void s_dump(FILE *io, pn_machine *m)
{
	fprintf(io, ".-----===( machine 0x%016lx )===-----.\n", (long)m);
	fprintf(io, "|                                              |\n");
	fprintf(io, "|     A: %8lx   B: %8lx   C: %8lx  |\n", m->A, m->B, m->C);
	fprintf(io, "|     D: %8lx   E: %8lx   F: %8lx  |\n", m->D, m->E, m->F);
	fprintf(io, "|                                              |\n");
	fprintf(io, "|    Ip: %8lx  S1: %8lx  S2: %8lx  |\n", m->Ip, m->S1, m->S2);
	fprintf(io, "|    Dp: %8lx   R: %8lx  Er: %8lx  |\n", m->Dp, m->R,  m->Er);
	fprintf(io, "|                                              |\n");
	fprintf(io, "|    T1: %8lx  T2: %8lx  Tr: %8lx  |\n", m->T1, m->T2, m->Tr);
	fprintf(io, "|                                              |\n");
if (m) {
	fprintf(io, "|  ----- PROGRAM ----------------------------  |\n");
	fprintf(io, "|                                              |\n");
	fprintf(io, "|  code: 0x%016lx     len: %-8u  |\n", (long)m->code, (unsigned int)m->codesize);
	fprintf(io, "|  data: 0x%016lx     len: %-8u  |\n", (long)m->data, (unsigned int)m->datasize);
} else {
	fprintf(io, "|        << no program code loaded >>          |\n");
}
	fprintf(io, "|                                              |\n");
	fprintf(io, "|  footprint: %8.2lfk                 v%04x  |\n", (sizeof(pn_machine) + m->codesize + m->datasize) / 1024.0, PENDULUM_VERSION);
	fprintf(io, "'----------------------------------------------'\n");
}

/**************************************************************/

int pn_init(pn_machine *m)
{
	memset(m, 0, sizeof(pn_machine));
	m->dump_fd = stderr;
	return 0;
}

int pn_destroy(pn_machine *m)
{
	m->A = m->B = m->C = 0;
	m->D = m->E = m->F = 0;
	m->Tr = m->R = m->E = 0;
	m->S1 = m->S2 = 0;
	m->T1 = m->T2 = 0;
	m->Ip = m->Dp = 0;

	pn_heap_purge(m);

	free(m->code);
	free(m->data);

	m->njumps = 0;  m->nfuncs = 0;  m->nflags = 0;
	free(m->jumps); free(m->funcs); free(m->flags);

	return 0;
}

int pn_set(pn_machine *m, int attr, void *value)
{
	switch (attr) {
	case PN_ATTR_DUMPFD:
		m->dump_fd = (FILE*)value;
		return 0;
	default:
		return -1;
	}
}

int pn_flag(pn_machine *m, const char *label, int value)
{
	pn_word flag = s_resolve_arg(m, label);
	if (!IS_FLAG(flag)) return 1;
	m->flags[TAGV(flag)].value = (value == 0 ? 0 : 1);
	pn_trace(m, TRACE_START " set flag %s = %i\n", TRACE_ARGS,
		m->flags[TAGV(flag)].label, m->flags[TAGV(flag)].value);
	return 0;
}

int pn_flagged(pn_machine *m, const char *label)
{
	pn_word flag = s_resolve_arg(m, label);
	if (!IS_FLAG(flag)) return 1;
	pn_trace(m, TRACE_START " flagged? %s (%i)\n", TRACE_ARGS,
		m->flags[TAGV(flag)].label, m->flags[TAGV(flag)].value);
	return !!(m->flags[TAGV(flag)].value);
}

int pn_func(pn_machine *m, const char *op, pn_function fn)
{
	assert(m);
	assert(op);
	assert(fn);

	int i = s_next_free_func_slot(m);
	if (i < 0) return -1;

	strncpy(m->funcs[i].name, op, 31);
	m->funcs[i].call = fn;
	return 0;
}

int pn_pragma(pn_machine *m, const char *name, const char *arg)
{
	return m->pragma ? (*m->pragma)(m, name, arg) : 1;
}

int pn_parse(pn_machine *m, FILE *io)
{
	assert(m);
	assert(io);

	char buf[1024], *op, *arg1, *arg2, *label;
	int i;

	fseek(io, 0, SEEK_SET);
	while (fgets(buf, 1024, io)) {
		if (s_label(buf, &label) == 0) {
			i = s_next_free_jump_slot(m);
			if (i < 0) {
				free(label);
				return 1;
			}
			strncpy(m->jumps[i].label, label, 63);
			m->jumps[i].step = m->codesize;
			free(label);
			m->codesize++;
			continue;
		}

		if (s_parse(buf, NULL, &arg1, &arg2) != 0) continue;

		m->codesize++;
		if (*arg1 == '"') m->datasize += strlen(arg1);
		if (*arg1 == '$') m->datasize += strlen(arg1);
		if (*arg2 == '"') m->datasize += strlen(arg2);
		if (*arg2 == '$') m->datasize += strlen(arg2);

		free(arg1); free(arg2);
		arg1 = arg2 = NULL;
	}
	m->codesize++;

	errno = ENOBUFS;
	m->code = calloc(m->codesize, sizeof(pn_opcode));
	m->data = calloc(m->datasize, sizeof(pn_byte));
	m->Dp = 0;
	if (!m->data || !m->code) return 1;

	rewind(io);
	while (fgets(buf, 1024, io)) {
		if (s_label(buf, NULL) == 0) {
			pn_CODE(m, m->Ip++).op = PN_OP_NOOP;
			continue;
		}

		if (s_parse(buf, &op, &arg1, &arg2) != 0) continue;

		pn_CODE(m, m->Ip).op = s_resolve_op(op);
		if (arg1) pn_CODE(m, m->Ip).arg1 = s_resolve_arg(m, arg1);
		if (arg2) pn_CODE(m, m->Ip).arg2 = s_resolve_arg(m, arg2);

		free(op);
		free(arg1); free(arg2);
		op = arg1 = arg2 = NULL;
		m->Ip++;
	}

	pn_CODE(m, m->Ip).op = PN_OP_HALT;
	m->Ip = 0;
	return 0;
}

int pn_trace(pn_machine *m, const char *fmt, ...)
{
	if (!m->trace) return 0;

	va_list ap;
	va_start(ap, fmt);

	int rc = vfprintf(m->dump_fd, fmt, ap);
	va_end(ap);

	return rc;
}

int pn_run(pn_machine *m)
{
	assert(m);

	m->Ip = 0;
#   define TEST(n,x,t1,op,t2) \
             if (!IS_LABEL(PC.arg1)) pn_die(m, "Invalid if-jump label for " n " operator"); \
             m->Tr = (x); \
             pn_trace(m, TRACE_START " %li " op " %li\n", TRACE_ARGS, t1, t2); \
             m->Dp = m->Tr ? TAGV(PC.arg1) : m->Ip + 1; \
             if (m->Tr) pn_trace(m, TRACE_START " branching to %p\n", TRACE_ARGS, m->Dp); \
             m->Ip = m->Dp; \
                 break

#   define TEST_S(n,mod,t1,op,t2) \
             if (!IS_LABEL(PC.arg1)) pn_die(m, "Invalid if-jump label for " n " operator"); \
             m->Tr = mod(strcmp((const char *)(t1), (const char *)(t2))); \
             pn_trace(m, TRACE_START " %s " op " %s\n", TRACE_ARGS, (const char *)(t1), (const char *)(t2)); \
             m->Dp = m->Tr ? TAGV(PC.arg1) : m->Ip + 1; \
             if (m->Tr) pn_trace(m, TRACE_START " branching to %p\n", TRACE_ARGS, m->Dp); \
             m->Ip = m->Dp; \
                 break

#   define TEST_F(n,mod) \
             if (!IS_FLAG(PC.arg1)) pn_die(m, "Non-flag argument to " n " operator"); \
             if (!IS_LABEL(PC.arg2)) pn_die(m, "Invalid if-jump label for " n "operator"); \
             pn_trace(m, TRACE_START " %s (%i)\n", TRACE_ARGS, \
                    m->flags[TAGV(PC.arg1)].label, m->flags[TAGV(PC.arg1)].value); \
             m->Tr = mod(m->flags[TAGV(PC.arg1)].value); \
             m->Dp = m->Tr ? TAGV(PC.arg2) : m->Ip + 1; \
             if (m->Tr) pn_trace(m, TRACE_START " branching to %p\n", TRACE_ARGS, m->Dp); \
             m->Ip = m->Dp; \
                 break

#   define NEXT    m->Ip++; break

	char *log;
	while (m->Ip < m->codesize) {
		switch (PC.op) {
		case PN_OP_NOOP:
			pn_trace(m, TRACE_START "\n", TRACE_ARGS);
			NEXT;

		case PN_OP_PRAGMA:
			pn_trace(m, TRACE_START " %s %s\n", TRACE_ARGS,
					(const char *)PC.arg1, (const char *)PC.arg2);
			m->R = pn_pragma(m, (const char *)PC.arg1, (const char *)PC.arg2);
			NEXT;

		case PN_OP_CMP:      TEST_S("CMP?",  !, m->T1, "eq", m->T2);
		case PN_OP_DIFF:     TEST_S("DIFF?", !!, m->T1, "ne", m->T2);
		case PN_OP_FLAGGED:  TEST_F("FLAGGED?",  !!);
		case PN_OP_NFLAGGED: TEST_F("!FLAGGED?", !);
		case PN_OP_EQ:       TEST("EQ?",    m->T1 == m->T2, m->T1, "==", m->T2);
		case PN_OP_NE:       TEST("NE?",    m->T1 != m->T2, m->T1, "!=", m->T2);
		case PN_OP_GT:       TEST("GT?",    m->T1 >  m->T2, m->T1, ">",  m->T2);
		case PN_OP_GTE:      TEST("GTE?",   m->T1 >= m->T2, m->T1, ">=", m->T2);
		case PN_OP_LT:       TEST("LT?",    m->T1 <  m->T2, m->T1, "<",  m->T2);
		case PN_OP_LTE:      TEST("LTE?",   m->T1 <= m->T2, m->T1, "<=", m->T2);
		case PN_OP_OK:       TEST("OK?",    m->R  == 0,     m->R,  "==", 0);
		case PN_OP_NOTOK:    TEST("NOTOK?", m->R  != 0,     m->R,  "!=", 0);

		case PN_OP_COPY:
			if (!IS_REGISTER(PC.arg1)) pn_die(m, "Non-register passed as source register to COPY operator");
			if (!IS_REGISTER(PC.arg2)) pn_die(m, "Non-register passed as destination register to COPY operator");
			pn_trace(m, TRACE_START " %%%s -> %%%s\n",
					TRACE_ARGS, REG_NAMES[TAGV(PC.arg1)], REG_NAMES[TAGV(PC.arg2)]);
			*(pn_word*)pn_REG(m, TAGV(PC.arg2)) = *(pn_word*)pn_REG(m, TAGV(PC.arg1));
			NEXT;

		case PN_OP_SET:
			if (!IS_REGISTER(PC.arg1)) pn_die(m, "Non-register passed as destination register to SET operator");
			pn_trace(m, TRACE_START " %%%s\n",
					TRACE_ARGS, REG_NAMES[TAGV(PC.arg1)]);
			*(pn_word*)pn_REG(m, TAGV(PC.arg1)) = TAGV((pn_word)PC.arg2);
			NEXT;

		case PN_OP_HALT:
			pn_trace(m, TRACE_START "\n", TRACE_ARGS);
			goto done;

		case PN_OP_ERROR:
			m->S1 = (pn_word)PC.arg1;
			pn_trace(m, TRACE_START " %s (errno=%i)\n", TRACE_ARGS, (const char *)m->S1, (int)m->Er);
			char *error = NULL;
			if (m->Er > 0)
				error = cw_string(": error %i [%s]", (int)m->Er, strerror((int)m->Er));

			log = cw_string("%s %s%s",
				m->topic ? m->topic : "(no topic)",
				(char *)m->S1, error ? error : "");
			free(error);

			cw_log(LOG_ERR, log, m->A, m->B, m->C, m->D, m->E, m->F);
			free(log);
			NEXT;

		case PN_OP_LOG:
			m->S1 = (pn_word)PC.arg2;
			pn_trace(m, TRACE_START " %s\n", TRACE_ARGS, (const char *)m->S1);
			log = cw_string("%s %s", m->topic ? m->topic : "(no topic)", (char *)m->S1);
			cw_log(PC.arg1, log, m->A, m->B, m->C, m->D, m->E, m->F);
			free(log);
			m->Ip++; break;
			NEXT;

		case PN_OP_CALL:
			if (!IS_FUNCTION(PC.arg1)) pn_die(m, "Non-function argument passed to CALL operator");
			pn_trace(m, TRACE_START " %s\n", TRACE_ARGS,
					(const char *)m->funcs[TAGV(PC.arg1)].name);
			m->R = (*m->funcs[TAGV(PC.arg1)].call)(m);
			pn_trace(m, TRACE_START " returned %#x\n", TRACE_ARGS,
					m->R);
			NEXT;

		case PN_OP_PRINT:
			m->S1 = (pn_word)PC.arg1;
			pn_trace(m, TRACE_START " %s\n", TRACE_ARGS, (const char *)m->S1);
			printf((char *)(m->S1), m->A, m->B, m->C, m->D, m->E, m->F);
			NEXT;

		case PN_OP_JUMP:
			if (!IS_LABEL(PC.arg1)) pn_die(m, "Non-label argument to JUMP operator");
			pn_trace(m, TRACE_START " %08lu\n", TRACE_ARGS, TAGV(PC.arg1));
			m->Ip = TAGV(PC.arg1);
			break;

		case PN_OP_DUMP:
			pn_trace(m, TRACE_START "\n", TRACE_ARGS);
			s_dump(m->dump_fd, m);
			NEXT;

		case PN_OP_TRACE:
			m->trace = PC.arg1 > 0 ? PC.arg1 : 0;
			fprintf(m->dump_fd, "--- %s TRACE ---\n", m->trace ? "ENABLE" : "DISABLE");
			NEXT;

		case PN_OP_VCHECK:
			if (!IS_LABEL(PC.arg2)) pn_die(m, "Invalid if-jump label for VCHECK operator"); \
			pn_trace(m, TRACE_START " v%i >= v%i\n", TRACE_ARGS, PENDULUM_VERSION, PC.arg1);
			m->Tr = (PENDULUM_VERSION >= PC.arg1);
			m->Dp = m->Tr ? TAGV(PC.arg2) : m->Ip + 1;
			if (m->Tr) pn_trace(m, TRACE_START " branching to %p\n", TRACE_ARGS, m->Dp);
			m->Ip = m->Dp;
			break;

		case PN_OP_SYSERR:
			pn_trace(m, TRACE_START " errno=%i\n", TRACE_ARGS, errno);
			fprintf(m->dump_fd, "SYSTEM ERROR %i: %s\n", errno, strerror(errno));
			NEXT;

		case PN_OP_FLAG:
			if (!IS_FLAG(PC.arg2)) pn_die(m, "Non-flag passed as second argument to FLAG operator");
			pn_trace(m, TRACE_START " %s = %i\n", TRACE_ARGS,
					m->flags[TAGV(PC.arg2)].label, PC.arg1 == 0 ? 0 : 1);
			m->flags[TAGV(PC.arg2)].value = (PC.arg1 ? 1 : 0);
			NEXT;

		case PN_OP_RESET:
			m->A = m->B = m->C = m->D = m->E = m->F = 0;
			m->T1 = m->T2 = m->R = 0;
			pn_flag(m, ":changed", 0);
			NEXT;

		case PN_OP_TOPIC:
			m->topics++;
			m->topic = (const char *)PC.arg1;
			pn_trace(m, TRACE_START " = '%s'\n", TRACE_ARGS, m->topic);
			NEXT;

		case PN_OP_GETENV:
			if (!IS_REGISTER(PC.arg1)) pn_die(m, "Non-register passed as destination register to GETENV operator");
			pn_trace(m, TRACE_START " $%s -> %%%s\n",
					TRACE_ARGS, TAGV((pn_word)PC.arg2), REG_NAMES[TAGV(PC.arg1)]);
			char *var = getenv((char*)TAGV((pn_word)PC.arg2));
			*(pn_word*)pn_REG(m, TAGV(PC.arg1)) = (pn_word)var;
			m->R = var ? 0 : 1;
			NEXT;

		case PN_OP_SETENV:
			pn_trace(m, TRACE_START " $%s -> '%s'\n",
					TRACE_ARGS, TAGV((pn_word)PC.arg1), TAGV((pn_word)PC.arg1));
			m->R = setenv((char*)TAGV((pn_word)PC.arg1),
			              (char*)TAGV((pn_word)PC.arg2), 1);
			NEXT;

		default: pn_die(m, "Unknown / Invalid operand");
		}
	}
done:
	return 0;
}

int pn_run_safe(pn_machine *m)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0)
		exit(pn_run(m));

	int rc;
	waitpid(pid, &rc, 0);
	if (rc != 0)
		cw_log(LOG_INFO, "pn_run %s, with rc %04x (%d)",
			WIFEXITED(rc) ? "exited" : "terminated abnormaly",
			rc, rc);
	return WIFEXITED(rc) ? WEXITSTATUS(rc) : -2;
}

int pn_die(pn_machine *m, const char *e)
{
	assert(m);
	assert(e);

	fprintf(stderr, "Fatal Pendulum Error: %s\n", e);
	exit(2);
}

int pn_heap_add(pn_machine *m, void *p)
{
	int i;
	for (i = 0; i < PN_HEAP_SIZE; i++) {
		if (m->heap[i] == p) {
			return 0;
		}
		if (!m->heap[i]) {
			m->heap[i] = p;
			return 0;
		}
	}
	return 1;
}

int pn_heap_purge(pn_machine *m)
{
	int i;
	for (i = 0; i < PN_HEAP_SIZE; i++) {
		if ((void*)m->A  == m->heap[i]) continue;
		if ((void*)m->B  == m->heap[i]) continue;
		if ((void*)m->C  == m->heap[i]) continue;
		if ((void*)m->D  == m->heap[i]) continue;
		if ((void*)m->E  == m->heap[i]) continue;
		if ((void*)m->F  == m->heap[i]) continue;

		if ((void*)m->S1 == m->heap[i]) continue;
		if ((void*)m->S2 == m->heap[i]) continue;

		if ((void*)m->T1 == m->heap[i]) continue;
		if ((void*)m->T2 == m->heap[i]) continue;

		free(m->heap[i]);
		m->heap[i] = NULL;
	}
	return 0;
}
