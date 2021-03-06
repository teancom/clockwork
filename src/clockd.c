/*
  Copyright 2011-2014 James Hunt <james@jameshunt.us>

  This file is part of Clockwork.

  Clockwork is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Clockwork is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Clockwork.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "clockwork.h"
#include <zmq.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <libgen.h>
#include <getopt.h>

#include "spec/parser.h"
#include "resources.h"
#include "pendulum.h"

#define BLOCK_SIZE 8192

#define PROTOCOL_VERSION         1
#define PROTOCOL_VERSION_STRING "1"

#define DEFAULT_CONFIG_FILE "/etc/clockwork/clockd.conf"

typedef enum {
	STATE_INIT,
	STATE_IDENTIFIED,
	STATE_COPYDOWN,
	STATE_POLICY,
	STATE_FILE,
	STATE_REPORT,
} state_t;

static const char *FSM_STATES[] = {
	"INIT",
	"IDENTIFIED",
	"COPYDOWN",
	"POLICY",
	"FILE",
	"REPORT",
	NULL,
};

typedef enum {
	EVENT_UNKNOWN,
	EVENT_PING,
	EVENT_HELLO,
	EVENT_COPYDOWN,
	EVENT_POLICY,
	EVENT_FILE,
	EVENT_DATA,
	EVENT_REPORT,
	EVENT_BYE,
} event_t;

static const char *FSM_EVENTS[] = {
	"UNKNOWN",
	"PING",
	"HELLO",
	"COPYDOWN",
	"POLICY",
	"FILE",
	"DATA",
	"REPORT",
	"BYE",
	NULL,
};

static event_t s_pdu_event(cw_pdu_t *pdu)
{
	int i;
	for (i = 0; FSM_EVENTS[i]; i++)
		if (strcmp(pdu->type, FSM_EVENTS[i]) == 0)
			return (event_t)i;
	return EVENT_UNKNOWN;
}

typedef enum {
	FSM_ERR_SUCCESS,
	FSM_ERR_INTERNAL,
	FSM_ERR_NOT_IMPLEMENTED,
	FSM_ERR_NO_POLICY_FOUND,
	FSM_ERR_NO_RESOURCE_FOUND,
	FSM_ERR_BADPROTO,
} fsm_err_t;

static const char *FSM_ERRORS[] = {
	"Success",
	"Internal Error",
	"Not Implemented",
	"Policy Not Found",
	"Resource Not Found",
	"Protocol Violation",
	NULL,
};

#define MODE_RUN  0
#define MODE_DUMP 1
#define MODE_TEST 2

typedef struct __client_t client_t;
typedef struct __server_t server_t;
typedef struct __ccache_t ccache_t;

struct __client_t {
	state_t           state;
	event_t           event;
	fsm_err_t         error;
	server_t         *server;

	cw_frame_t       *ident;
	char             *rident;
	int32_t           last_seen;

	char             *name;
	struct stree     *pnode;
	struct policy    *policy;
	cw_hash_t        *facts;

	FILE             *io;
	unsigned long     offset;
	struct SHA1       sha1;

	void             *mapped;
	size_t            maplen;
};

struct __ccache_t {
	size_t len;
	int32_t min_life;
	client_t clients[];
};

struct __server_t {
	void *zmq;
	void *listener;

	int mode;
	int daemonize;

	ccache_t *ccache;
	struct manifest  *manifest;
	char             *copydown;

	cw_cert_t    *cert;
	cw_trustdb_t *tdb;
	void         *zap;
};

static int s_sha1(client_t *fsm)
{
	struct sha1_ctx ctx;
	sha1_init(&fsm->sha1, NULL);
	sha1_ctx_init(&ctx);

	char data[BLOCK_SIZE];
	size_t n;
	int fd = fileno(fsm->io);
	while ((n = read(fd, data, BLOCK_SIZE)) > 0)
		sha1_ctx_update(&ctx, (uint8_t *)data, n);
	sha1_ctx_final(&ctx, fsm->sha1.raw);
	sha1_hexdigest(&fsm->sha1);

	rewind(fsm->io);
	return 0;
}

static char * s_gencode(client_t *c)
{
	FILE *io = tmpfile();
	assert(io);

	cw_timer_t t;
	uint32_t ms = 0;
	TIMER(&t, ms) {
		policy_gencode(c->policy, io);
	}

	fprintf(io, "%c", '\0');
	c->maplen = ftell(io);
	c->mapped = mmap(NULL, c->maplen, PROT_READ, MAP_SHARED, fileno(io), 0);
	fclose(io);

	if (c->mapped == MAP_FAILED)
		c->mapped = NULL;

	float size;
	char unit = 'b';
	if (c->maplen > 1024 * 1024 * 1024) {
		size = c->maplen / 1024.0 / 1024.0 / 1024.0;
		unit = 'G';
	} else if (c->maplen > 1024 * 1024) {
		size = c->maplen / 1024.0 / 1024.0;
		unit = 'M';
	} else if (c->maplen > 1024) {
		size = c->maplen / 1024.0;
		unit = 'k';
	} else {
		size = c->maplen;
		unit = 'b';
	}
	cw_log(LOG_INFO, "generated %0.2f%c policy for %s/%s in %lums", size, unit, c->rident, c->name, ms);

	return (char *)c->mapped;
}

static int s_state_machine(client_t *fsm, cw_pdu_t *pdu, cw_pdu_t **reply)
{
	fsm->last_seen = cw_time_s();

	cw_log(LOG_DEBUG, "fsm: transition %s [%i] -> %s [%i]",
			FSM_STATES[fsm->state], fsm->state,
			FSM_EVENTS[fsm->event], fsm->event);

	switch (fsm->event) {
	case EVENT_UNKNOWN:
		fsm->error = FSM_ERR_BADPROTO;
		return 1;

	case EVENT_PING:
		*reply = cw_pdu_make(pdu->src, 2, "PONG", PROTOCOL_VERSION_STRING);
		return 0;

	case EVENT_HELLO:
		switch (fsm->state) {
		case STATE_FILE:
		case STATE_COPYDOWN:
			if (fsm->io) fclose(fsm->io);
			fsm->io = NULL;
			fsm->offset = 0;

		case STATE_POLICY:
			cw_hash_done(fsm->facts, 1);
			fsm->facts = NULL;
			policy_free(fsm->policy);
			fsm->policy = NULL;

		case STATE_IDENTIFIED:
			free(fsm->name);

		case STATE_INIT:
		case STATE_REPORT:
			/* fall-through */
			break;
		}

		/* FIXME: send back pendulum code for gatherer setup */
		fsm->name = cw_pdu_text(pdu, 1);
		*reply = cw_pdu_make(pdu->src, 1, "OK");

		fsm->state = STATE_IDENTIFIED;
		return 0;

	case EVENT_COPYDOWN:
		switch (fsm->state) {
		case STATE_INIT:
		case STATE_REPORT:
		case STATE_FILE:
		case STATE_COPYDOWN:
		case STATE_POLICY:
			fsm->error = FSM_ERR_BADPROTO;
			return 1;

		case STATE_IDENTIFIED:
			fsm->io = tmpfile();
			if (!fsm->io) {
				cw_log(LOG_ERR, "unable to create a temporary file for the copydown archive: %s",
					strerror(errno));
				fsm->error = FSM_ERR_INTERNAL;
				return 1;
			}

			if (cw_bdfa_pack(fileno(fsm->io), fsm->server->copydown) != 0) {
				cw_log(LOG_ERR, "unable to pack the copydown archive: %s",
					strerror(errno));
				fsm->error = FSM_ERR_INTERNAL;
				return 1;
			}
			rewind(fsm->io);
			fsm->offset = 0;

			*reply = cw_pdu_make(pdu->src, 1, "OK");
			break;
		}

		fsm->state = STATE_COPYDOWN;
		return 0;

	case EVENT_POLICY:
		switch (fsm->state) {
		case STATE_INIT:
			fsm->error = FSM_ERR_BADPROTO;
			return 1;

		case STATE_REPORT:
		case STATE_FILE:
		case STATE_COPYDOWN:
			if (fsm->io) fclose(fsm->io);
			fsm->io = NULL;
			fsm->offset = 0;

		case STATE_POLICY:
			cw_hash_done(fsm->facts, 1);
			fsm->facts = NULL;
			policy_free(fsm->policy);
			fsm->policy = NULL;

		case STATE_IDENTIFIED:
			/* fall-through */
			break;
		}

		char *facts = cw_pdu_text(pdu, 2);
		fsm->facts = cw_alloc(sizeof(cw_hash_t));
		fact_read_string(facts, fsm->facts);
		free(facts);

		fsm->pnode = cw_hash_get(fsm->server->manifest->hosts, fsm->name);
		if (!fsm->pnode) fsm->pnode = fsm->server->manifest->fallback;
		if (!fsm->pnode) {
			fsm->error = FSM_ERR_NO_POLICY_FOUND;
			return 1;
		}
		fsm->policy = policy_generate(fsm->pnode, fsm->facts);

		char *code = s_gencode(fsm);
		*reply = cw_pdu_make(pdu->src, 2, "POLICY", code);
		fsm->state = STATE_POLICY;
		return 0;

	case EVENT_FILE:
		switch (fsm->state) {
		case STATE_INIT:
		case STATE_IDENTIFIED:
		case STATE_COPYDOWN:
		case STATE_REPORT:
			fsm->error = FSM_ERR_BADPROTO;
			return 1;

		case STATE_FILE:
			if (fsm->io) fclose(fsm->io);
			fsm->io = NULL;
			fsm->offset = 0;

		case STATE_POLICY:
			/* fall-through */
			break;
		}

		/* open the file, calculate the SHA1 */
		char *key = cw_pdu_text(pdu, 1);
		struct resource *r = cw_hash_get(fsm->policy->index, key);
		free(key);

		if (!r) {
			fsm->error = FSM_ERR_NO_RESOURCE_FOUND;
			return 1;
		}

		fsm->io = resource_content(r, fsm->facts);
		if (!fsm->io) {
			cw_log(LOG_ERR, "failed to generate content for %s (on behalf of %s): %s",
				r->key, fsm->name, strerror(errno));
			fsm->error = FSM_ERR_INTERNAL;
			return 1;
		}

		s_sha1(fsm);
		*reply = cw_pdu_make(pdu->src, 2, "SHA1", fsm->sha1.hex);
		fsm->state = STATE_FILE;
		return 0;

	case EVENT_DATA:
		switch (fsm->state) {
		case STATE_INIT:
		case STATE_IDENTIFIED:
		case STATE_POLICY:
		case STATE_REPORT:
			fsm->error = FSM_ERR_BADPROTO;
			return 1;

		case STATE_FILE:
		case STATE_COPYDOWN:
			/* fall-through */
			break;
		}

		char *off = cw_pdu_text(pdu, 1);
		fsm->offset = BLOCK_SIZE * atoi(off);
		free(off);

		char block[BLOCK_SIZE+1] = "";
		fseek(fsm->io, fsm->offset, SEEK_SET);
		size_t n = fread(block, 1, BLOCK_SIZE, fsm->io);
		block[n] = '\0';

		if (n == 0) {
			if (feof(fsm->io)) {
				*reply = cw_pdu_make(pdu->src, 1, "EOF");
			} else {
				cw_log(LOG_ERR, "Failed to read from cached IO handled: %s", strerror(errno));
				*reply = cw_pdu_make(pdu->src, 2, "ERROR", "read error");
			}

		} else {
			*reply = cw_pdu_make(pdu->src, 1, "BLOCK");
			assert(*reply);

			/* because block may have embedded NULLs, we have
			   to manufacture our frame the old fashioned way */
			cw_frame_t *dframe = cw_frame_newbuf(block, n);
			assert(dframe);
			cw_pdu_extend(*reply, dframe);
		}
		return 0;

	case EVENT_REPORT:
		switch (fsm->state) {
		case STATE_INIT:
		case STATE_IDENTIFIED:
			fsm->error = FSM_ERR_BADPROTO;
			return 1;

		case STATE_FILE:
		case STATE_COPYDOWN:
			if (fsm->io) fclose(fsm->io);
			fsm->io = NULL;
			fsm->offset = 0;

		case STATE_POLICY:
		case STATE_REPORT:
			/* fall-through */
			break;
		}
		break;

	case EVENT_BYE:
		switch (fsm->state) {
		case STATE_REPORT:
		case STATE_FILE:
		case STATE_COPYDOWN:
			if (fsm->io) fclose(fsm->io);
			fsm->io = NULL;
			fsm->offset = 0;

		case STATE_POLICY:
			cw_hash_done(fsm->facts, 1);
			free(fsm->facts);
			fsm->facts = NULL;
			policy_free_all(fsm->policy);
			fsm->policy = NULL;

		case STATE_IDENTIFIED:
			free(fsm->name);
			fsm->name = NULL;

		case STATE_INIT:
			/* fall-through */
			break;
		}

		fsm->state = STATE_INIT;
		fsm->last_seen = 1;
		*reply = cw_pdu_make(pdu->src, 1, "BYE");
		return 0;
	}

	fsm->error = FSM_ERR_INTERNAL;
	return 1;
}

static ccache_t* s_ccache_init(size_t num_conns, int32_t min_life)
{
	ccache_t *cc = cw_alloc(
			sizeof(ccache_t) +
			(num_conns * sizeof(client_t)));
	cc->len      = num_conns;
	cc->min_life = min_life;
	return cc;
}

static void s_ccache_purge(ccache_t *c, int force)
{
	int32_t now = cw_time_s();
	int n = 0;
	size_t i;
	for (i = 0; i < c->len; i++) {
		if (!force &&
		     (c->clients[i].last_seen == 0
		   || c->clients[i].last_seen >= now - c->min_life))
			continue;

		cw_log(LOG_DEBUG, "purging %p, last_seen=%i, name=%s, %s facts, %s policy",
			&c->clients[i], c->clients[i].last_seen, c->clients[i].name,
			(c->clients[i].facts  ? "has" : "no"),
			(c->clients[i].policy ? "has" : "no"));

		if (c->clients[i].ident) {
			cw_frame_close(c->clients[i].ident);
			free(c->clients[i].ident);
			c->clients[i].ident = NULL;
		}
		free(c->clients[i].rident);
		c->clients[i].rident = NULL;

		free(c->clients[i].name);
		c->clients[i].name = NULL;

		if (c->clients[i].facts) {
			cw_hash_done(c->clients[i].facts, 1);
			free(c->clients[i].facts);
			c->clients[i].facts = NULL;
		}

		if (c->clients[i].io)
			fclose(c->clients[i].io);
		c->clients[i].io        = NULL;
		c->clients[i].offset    = 0;
		c->clients[i].last_seen = 0;

		policy_free_all(c->clients[i].policy);
		c->clients[i].policy = NULL;
		n++;
	}
	if (n) cw_log(LOG_INFO, "purged %i ccache entries", n);
}

static void s_ccache_destroy(ccache_t *c)
{
	s_ccache_purge(c, 1);
	free(c);
}

static size_t s_ccache_index(cw_frame_t *ident, ccache_t *c)
{
	size_t i;
	for (i = 0; i < c->len; i++)
		if (c->clients[i].ident
		 && cw_frame_cmp(ident, c->clients[i].ident))
			break;
	return i;
}

static size_t s_ccache_next(ccache_t *c)
{
	size_t i;
	for (i = 0; i < c->len; i++)
		if (!c->clients[i].ident)
			break;
	return i;
}

static client_t* s_ccache_find(cw_frame_t *ident, ccache_t *c)
{
	size_t i = s_ccache_index(ident, c);
	if (i >= c->len) {
		i = s_ccache_next(c);
		if (i >= c->len)
			return NULL;
		c->clients[i].ident  = cw_frame_copy(ident);
		c->clients[i].rident = cw_frame_hex(ident);
		c->clients[i].io = NULL;
		c->clients[i].offset = 0;
		cw_log(LOG_INFO, "new inbound connection from client %s, assigning index %i",
			c->clients[i].rident, i);
	}
	c->clients[i].last_seen = cw_time_s();
	return &c->clients[i];
}

static inline server_t *s_server_new(int argc, char **argv)
{
	char *t;
	server_t *s = cw_alloc(sizeof(server_t));
	s->daemonize = 1;

	LIST(config);
	cw_cfg_set(&config, "listen",              "*:2314");
	cw_cfg_set(&config, "ccache.connections",  "2048");
	cw_cfg_set(&config, "ccache.expiration",   "600");
	cw_cfg_set(&config, "manifest",            "/etc/clockwork/manifest.pol");
	cw_cfg_set(&config, "copydown",            CW_GATHER_DIR);
	cw_cfg_set(&config, "syslog.ident",        "clockd");
	cw_cfg_set(&config, "syslog.facility",     "daemon");
	cw_cfg_set(&config, "syslog.level",        "error");
	cw_cfg_set(&config, "security.strict",     "yes");
	cw_cfg_set(&config, "security.trusted",    "/etc/clockwork/certs/trusted");
	cw_cfg_set(&config, "security.cert",       "/etc/clockwork/certs/clockd");
	cw_cfg_set(&config, "pidfile",             "/var/run/clockd.pid");

	cw_log_open(cw_cfg_get(&config, "syslog.ident"), "stderr");
	cw_log_level(0, (getenv("CLOCKD_DEBUG") ? "debug" : "error"));
	cw_log(LOG_DEBUG, "default configuration:");
	cw_log(LOG_DEBUG, "  listen              %s", cw_cfg_get(&config, "listen"));
	cw_log(LOG_DEBUG, "  ccache.connections  %s", cw_cfg_get(&config, "ccache.connections"));
	cw_log(LOG_DEBUG, "  ccache.expiration   %s", cw_cfg_get(&config, "ccache.expiration"));
	cw_log(LOG_DEBUG, "  manifest            %s", cw_cfg_get(&config, "manifest"));
	cw_log(LOG_DEBUG, "  copydown            %s", cw_cfg_get(&config, "copydown"));
	cw_log(LOG_DEBUG, "  syslog.ident        %s", cw_cfg_get(&config, "syslog.ident"));
	cw_log(LOG_DEBUG, "  syslog.facility     %s", cw_cfg_get(&config, "syslog.facility"));
	cw_log(LOG_DEBUG, "  syslog.level        %s", cw_cfg_get(&config, "syslog.level"));
	cw_log(LOG_DEBUG, "  security.strict     %s", cw_cfg_get(&config, "security.strict"));
	cw_log(LOG_DEBUG, "  security.trusted    %s", cw_cfg_get(&config, "security.trusted"));
	cw_log(LOG_DEBUG, "  security.cert       %s", cw_cfg_get(&config, "security.cert"));
	cw_log(LOG_DEBUG, "  pidfile             %s", cw_cfg_get(&config, "pidfile"));


	cw_log(LOG_DEBUG, "processing command-line options");
	const char *short_opts = "?hvqVFSt" "c:";
	struct option long_opts[] = {
		{ "help",        no_argument,       NULL, 'h' },
		{ "verbose",     no_argument,       NULL, 'v' },
		{ "quiet",       no_argument,       NULL, 'q' },
		{ "version",     no_argument,       NULL, 'V' },
		{ "foreground",  no_argument,       NULL, 'F' },
		{ "show-config", no_argument,       NULL, 'S' },
		{ "test",        no_argument,       NULL, 't' },
		{ "config",      required_argument, NULL, 'c' },
		{ 0, 0, 0, 0 },
	};
	int verbose = -1;
	const char *config_file = DEFAULT_CONFIG_FILE;
	int opt, idx = 0;
	while ( (opt = getopt_long(argc, argv, short_opts, long_opts, &idx)) != -1) {
		switch (opt) {
		case 'h':
		case '?':
			cw_log(LOG_DEBUG, "handling -h/-?/--help");
			printf("clockd, part of clockwork v%s runtime %i protocol %i\n",
				PACKAGE_VERSION, PENDULUM_VERSION, PROTOCOL_VERSION);
			printf("Usage: clockd [-?hvqVFSt] [-c filename]\n\n");
			printf("Options:\n");
			printf("  -?, -h               show this help screen\n");
			printf("  -V, --version        show version information and exit\n");
			printf("  -v, --verbose        increase logging verbosity\n");
			printf("  -q, --quiet          disable logging\n");
			printf("  -F, --foreground     don't daemonize, run in the foreground\n");
			printf("  -S, --show-config    print configuration and exit\n");
			printf("  -t, --test           test configuration and manifest\n");
			printf("  -c filename          set configuration file (default: " DEFAULT_CONFIG_FILE ")\n");
			exit(0);

		case 'v':
			if (verbose < 0) verbose = 0;
			verbose++;
			cw_log(LOG_DEBUG, "handling -v/--verbose (modifier = %i)", verbose);
			break;

		case 'q':
			verbose = 0;
			cw_log(LOG_DEBUG, "handling -q/--quiet (modifier = %i)", verbose);
			break;

		case 'V':
			cw_log(LOG_DEBUG, "handling -V/--version");
			printf("clockd (Clockwork) %s runtime v%04x\n"
			       "Copyright (C) 2014 James Hunt\n",
			       PACKAGE_VERSION, PENDULUM_VERSION);
			exit(0);
			break;

		case 'c':
			cw_log(LOG_DEBUG, "handling -c/--config; replacing '%s' with '%s'",
				config_file, optarg);
			config_file = optarg;
			break;

		case 'F':
			cw_log(LOG_DEBUG, "handling -F/--foreground; turning off daemonize behavior");
			s->daemonize = 0;
			break;

		case 'S':
			cw_log(LOG_DEBUG, "handling -S/--show-config");
			s->mode = MODE_DUMP;
			break;

		case 't':
			cw_log(LOG_DEBUG, "handling -t/--test");
			s->mode = MODE_TEST;
			break;
		}
	}
	cw_log(LOG_DEBUG, "option processing complete");


	cw_log(LOG_DEBUG, "parsing clockd configuration file '%s'", config_file);
	FILE *io = fopen(config_file, "r");
	if (!io) {
		cw_log(LOG_WARNING, "Failed to read configuration from %s: %s",
			config_file, strerror(errno));
		cw_log(LOG_WARNING, "Using default configuration");

	} else {
		if (cw_cfg_read(&config, io) != 0) {
			cw_log(LOG_ERR, "Unable to parse %s");
			exit(1);
		}
		fclose(io);
		io = NULL;
	}


	cw_log(LOG_DEBUG, "determining adjusted log level/facility");
	if (verbose < 0) verbose = 0;
	t = cw_cfg_get(&config, "syslog.level");
	cw_log(LOG_DEBUG, "configured log level is '%s', verbose modifier is %+i", t, verbose);
	int level = cw_log_level_number(t);
	if (level < 0) {
		cw_log(LOG_WARNING, "'%s' is not a recognized log level, falling back to 'error'", t);
		level = LOG_ERR;
	}
	level += verbose;
	cw_log(LOG_DEBUG, "adjusted log level is %s (%i)",
		cw_log_level_name(level), level);
	if (!s->daemonize) {
		cw_log(LOG_DEBUG, "Running in --foreground mode; forcing all logging to stdout");
		cw_cfg_set(&config, "syslog.facility", "stdout");
	}
	if (s->mode == MODE_DUMP) {
		cw_log(LOG_DEBUG, "Running in --show-config mode; forcing all logging to stderr");
		cw_cfg_set(&config, "syslog.facility", "stderr");
	}
	if (s->mode == MODE_TEST) {
		cw_log(LOG_DEBUG, "Running in --test mode; forcing all logging to stderr");
		cw_cfg_set(&config, "syslog.facility", "stderr");
	}
	cw_log(LOG_DEBUG, "redirecting to %s log as %s",
		cw_cfg_get(&config, "syslog.facility"),
		cw_cfg_get(&config, "syslog.ident"));

	cw_log_open(cw_cfg_get(&config, "syslog.ident"),
	            cw_cfg_get(&config, "syslog.facility"));
	cw_log_level(level, NULL);

	cw_log(LOG_INFO, "clockd starting up");


	if (s->mode == MODE_DUMP) {
		printf("listen              %s\n", cw_cfg_get(&config, "listen"));
		printf("ccache.connections  %s\n", cw_cfg_get(&config, "ccache.connections"));
		printf("ccache.expiration   %s\n", cw_cfg_get(&config, "ccache.expiration"));
		printf("manifest            %s\n", cw_cfg_get(&config, "manifest"));
		printf("copydown            %s\n", cw_cfg_get(&config, "copydown"));
		printf("syslog.ident        %s\n", cw_cfg_get(&config, "syslog.ident"));
		printf("syslog.facility     %s\n", cw_cfg_get(&config, "syslog.facility"));
		printf("syslog.level        %s\n", cw_cfg_get(&config, "syslog.level"));
		printf("security.strict     %s\n", cw_cfg_get(&config, "security.strict"));
		printf("security.trusted    %s\n", cw_cfg_get(&config, "security.trusted"));
		printf("security.cert       %s\n", cw_cfg_get(&config, "security.cert"));
		printf("pidfile             %s\n", cw_cfg_get(&config, "pidfile"));
		exit(0);
	}


	s->cert = cw_cert_read(cw_cfg_get(&config, "security.cert"));
	if (!s->cert) {
		cw_log(LOG_ERR, "%s: %s", cw_cfg_get(&config, "security.cert"),
			errno == EINVAL ? "Invalid Clockwork certificate" : strerror(errno));
		exit(1);
	}
	if (!s->cert->seckey) {
		cw_log(LOG_ERR, "%s: No secret key found in certificate",
			cw_cfg_get(&config, "security.cert"));
		exit(1);
	}
	if (!s->cert->ident) {
		cw_log(LOG_ERR, "%s: No identity found in certificate",
			cw_cfg_get(&config, "security.cert"));
		exit(1);
	}

	s->tdb = cw_trustdb_read(cw_cfg_get(&config, "security.trusted"));
	if (!s->tdb) s->tdb = cw_trustdb_new();
	s->tdb->verify = strcmp(cw_cfg_get(&config, "security.strict"), "no");
	cw_log(LOG_DEBUG, "%s certificate verification",
		s->tdb->verify ? "Enabling" : "Disabling");

	s->copydown = strdup(cw_cfg_get(&config, "copydown"));
	s->manifest = parse_file(cw_cfg_get(&config, "manifest"));
	if (!s->manifest) {
		if (errno)
			cw_log(LOG_CRIT, "Failed to parse %s: %s",
				cw_cfg_get(&config, "manifest"), strerror(errno));
		exit(1);
	}
	if (s->mode == MODE_TEST) {
		if (manifest_validate(s->manifest) != 0)
			exit(2);
		printf("Syntax OK\n");
		exit(0);
	}

	if (s->daemonize)
		cw_daemonize(cw_cfg_get(&config, "pidfile"), "root", "root");
	s->ccache = s_ccache_init(atoi(cw_cfg_get(&config, "ccache.connections")),
	                          atoi(cw_cfg_get(&config, "ccache.expiration")));


	s->zmq = zmq_ctx_new();
	s->zap = cw_zap_startup(s->zmq, s->tdb);

	t = cw_string("tcp://%s", cw_cfg_get(&config, "listen"));
	cw_log(LOG_DEBUG, "binding to %s", t);
	s->listener = zmq_socket(s->zmq, ZMQ_ROUTER);

	int rc, optval = 1;
	cw_log(LOG_DEBUG, "Setting ZMQ_CURVE_SERVER to %i", optval);
	rc = zmq_setsockopt(s->listener, ZMQ_CURVE_SERVER, &optval, sizeof(optval));
	assert(rc == 0);
	cw_log(LOG_DEBUG, "Setting ZMQ_CURVE_SECRETKEY (sec) to %s", s->cert->seckey_b16);
	rc = zmq_setsockopt(s->listener, ZMQ_CURVE_SECRETKEY, cw_cert_secret(s->cert), 32);
	assert(rc == 0);

	rc = zmq_bind(s->listener, t);
	assert(rc == 0);
	free(t);

	cw_cfg_done(&config);

	cw_log(LOG_INFO, "clockd running");
	return s;
}

static inline void s_server_destroy(server_t *s)
{
	cw_zap_shutdown(s->zap);
	s_ccache_destroy(s->ccache);
	manifest_free(s->manifest);
	cw_cert_destroy(s->cert);
	cw_trustdb_destroy(s->tdb);
	free(s->copydown);
	free(s);
}

int main(int argc, char **argv)
{
#ifdef UNIT_TESTS
	/* only let unit tests run for 60s */
	alarm(60);
#endif
	server_t *s = s_server_new(argc, argv);
#ifdef UNIT_TESTS
	if (getenv("TEST_CLOCKD_BAIL_EARLY"))
		exit(0);
#endif
	cw_sig_catch();
	while (!cw_sig_interrupt()) {
		cw_log(LOG_DEBUG, "awaiting inbound connection");
		cw_pdu_t *pdu, *reply;
		pdu = cw_pdu_recv(s->listener);
		if (!pdu) continue;

		cw_log(LOG_DEBUG, "received inbound connection, checking ccache for client details");
		s_ccache_purge(s->ccache, 0);
		client_t *c = s_ccache_find(pdu->src, s->ccache);
		if (!c) {
			cw_log(LOG_CRIT, "max connections reached!");
			reply = cw_pdu_make(pdu->src, 2, "ERROR", "Server busy; try again later\n");
			cw_pdu_send(s->listener, reply);
			cw_pdu_destroy(pdu);
			cw_pdu_destroy(reply);
			continue;
		}
		cw_log(LOG_DEBUG, "inbound connection for client %p", c);

		c->server = s;
		c->event = s_pdu_event(pdu);
		int rc = s_state_machine(c, pdu, &reply);
		if (rc == 0) {
			cw_log(LOG_DEBUG, "%s: fsm is now at %s [%i]", pdu->client, FSM_STATES[c->state], c->state);
			cw_log(LOG_DEBUG, "%s: sending back a %s PDU", pdu->client, reply->type);
			cw_pdu_send(s->listener, reply);
			cw_pdu_destroy(pdu);
			cw_pdu_destroy(reply);

			if (c->mapped) {
				munmap(c->mapped, c->maplen);
				c->mapped = NULL;
				c->maplen = 0;
			}

#ifdef UNIT_TESTS
			if (c->event == EVENT_BYE)
				goto unit_tests_finished;
#endif

		} else {
			reply = cw_pdu_make(pdu->src, 2, "ERROR", FSM_ERRORS[c->error]);
			cw_log(LOG_DEBUG, "%s: sending back an ERROR PDU: %s", pdu->client, FSM_ERRORS[c->error]);
			cw_pdu_send(s->listener, reply);
			cw_pdu_destroy(pdu);
			cw_pdu_destroy(reply);

#ifdef UNIT_TESTS
			goto unit_tests_finished;
#endif
		}
	}

#ifdef UNIT_TESTS
unit_tests_finished:
#endif
	cw_log(LOG_INFO, "shutting down");

	cw_zmq_shutdown(s->listener, 500);
	zmq_ctx_destroy(s->zmq);
	s_server_destroy(s);

	cw_log_close();
	return 0;
}
