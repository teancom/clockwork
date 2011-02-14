#include "clockwork.h"

#include <getopt.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "cert.h"
#include "spec/parser.h"
#include "server.h"

/**************************************************************/

typedef struct {
	BIO *socket;
	SSL *ssl;
	THREAD_TYPE tid;

	char *peer;

	char *requests_dir;
	char *certs_dir;

	struct hash   *facts;
	struct policy *policy;
	struct stree  *pnode;

	protocol_session session;

	struct hash *config;

	unsigned short peer_verified;
} worker;

/**************************************************************/

static const char      *manifest_file = NULL;
static struct manifest *manifest = NULL;
static pthread_mutex_t  manifest_mutex = PTHREAD_MUTEX_INITIALIZER;

/**************************************************************/

static server* policyd_options(int argc, char **argv);
static void show_help(void);

static worker* worker_spawn(server *s);
static int worker_prep(worker *w);
static int worker_dispatch(worker *w);
static void worker_die(worker *w);
static int worker_verify_peer(worker *w);
static int worker_send_file(worker *w, const char *path);

static int handle_hello(worker *w);
static int handle_get_cert(worker *w);
static int handle_get_policy(worker *w);
static int handle_get_file(worker *w);
static int handle_put_report(worker *w);
static int handle_unknown(worker *w);

static void server_sighup(int, siginfo_t*, void*);

static void daemonize(const char *lock_file, const char *pid_file);

static int server_init_ssl(server *s);
static int server_init(server *s);

static void* server_worker_thread(void *arg);
static void* server_manager_thread(void *arg);

/**************************************************************/

int main(int argc, char **argv)
{
	server *s;
	worker *w;

	s = policyd_options(argc, argv);
	if (server_options(s) != 0) {
		fprintf(stderr, "Unable to process server options");
		exit(2);
	}

	INFO("policyd starting up");
	if (server_init(s) != 0) {
		CRITICAL("Failed to initialize policyd server thread");
		exit(2);
	}
	DEBUG("entering server_loop");
	for (;;) {
		w = worker_spawn(s);
		if (!w) {
			WARNING("Failed to spawn worker for inbound connection");
			continue;
		}
		THREAD_CREATE(w->tid, server_worker_thread, w);
	}

	SSL_CTX_free(s->ssl_ctx);
	return 0;
}

/**************************************************************/

static server* policyd_options(int argc, char **argv)
{
	server *s;

	const char *short_opts = "h?FDvqQc:p:";
	struct option long_opts[] = {
		{ "help",         no_argument,       NULL, 'h' },
		{ "no-daemonize", no_argument,       NULL, 'F' },
		{ "foreground",   no_argument,       NULL, 'F' },
		{ "debug",        no_argument,       NULL, 'D' },
		{ "silent",       no_argument,       NULL, 'Q' },
		{ "config",       required_argument, NULL, 'c' },
		{ "port",         required_argument, NULL, 'p' },
		{ 0, 0, 0, 0 },
	};

	int opt, idx = 0;

	s = xmalloc(sizeof(server));

	while ( (opt = getopt_long(argc, argv, short_opts, long_opts, &idx)) != -1 ) {
		switch (opt) {
		case 'h':
		case '?':
			show_help();
			exit(0);
		case 'F':
			s->daemonize = SERVER_OPT_FALSE;
			break;
		case 'D':
			s->daemonize = SERVER_OPT_FALSE;
			s->debug = 1;
			break;
		case 'v':
			s->log_level++;
			break;
		case 'q':
			s->log_level--;
			break;
		case 'Q':
			s->log_level = 0;
			break;
		case 'c':
			free(s->config_file);
			s->config_file = strdup(optarg);
			break;
		case 'p':
			free(s->port);
			s->port = strdup(optarg);
			break;
		}
	}

	return s;
}

static void show_help(void)
{
	printf( "USAGE: policyd [OPTIONS]\n"
	       "\n"
	       "  -h, --help            Show this helpful message.\n"
	       "                        (for more in-depth help, check the man pages.)\n"
	       "\n"
	       "  -F, --foreground      Do not fork into the background.\n"
	       "                        Useful for debugging with -D\n"
	       "\n"
	       "  -D, --debug           Run in debug mode; log to stderr instead of\n"
	       "                        syslog.  Implies -F.\n"
	       "\n"
	       "  -v[vvv...]            Increase verbosity by one level.  Can be used\n"
	       "                        more than once.  See -Q and -q.\n"
	       "\n"
	       "  -q[qqq...]            Decrease verbosity by one level.  Can be used\n"
	       "                        more than once.  See -v and -Q.\n"
	       "\n"
	       "  -Q, --silent          Reset verbosity to the default setting, such that\n"
	       "                        only CRITICAL and ERROR messages are logged, and all\n"
	       "                        others are discarded.  See -q and -v.\n"
	       "\n"
	       "  -c, --config          Specify the path to an alternate configuration file.\n"
	       "\n"
	       "  -p, --port            Override the TCP port number that policyd should\n"
	       "                        bind to and listen on.\n"
	       "\n");
}

static worker* worker_spawn(server *s)
{
	worker *w;

	if (BIO_do_accept(s->listener) <= 0) {
		WARNING("Couldn't accept inbound connection");
		protocol_ssl_backtrace();
		return NULL;
	}

	w = xmalloc(sizeof(worker));
	w->facts = hash_new();
	w->peer_verified = 0;

	w->requests_dir = strdup(s->requests_dir);
	w->certs_dir    = strdup(s->certs_dir);

	w->socket = BIO_pop(s->listener);
	if (!(w->ssl = SSL_new(s->ssl_ctx))) {
		WARNING("Couldn't create SSL handle for worker thread");
		protocol_ssl_backtrace();

		free(w);
		return NULL;
	}

	SSL_set_bio(w->ssl, w->socket, w->socket);
	return w;
}

static int worker_prep(worker *w)
{
	assert(w);

	sigset_t blocked_signals;

	/* block SIGPIPE for clients that close early. */
	sigemptyset(&blocked_signals);
	sigaddset(&blocked_signals, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &blocked_signals, NULL);

	if (SSL_accept(w->ssl) <= 0) {
		ERROR("Unable to establish SSL connection");
		protocol_ssl_backtrace();
		return -1;
	}

	if (worker_verify_peer(w) != 0) {
		return -1;
	}

	protocol_session_init(&w->session, w->ssl);

	pthread_mutex_lock(&manifest_mutex);
		w->pnode = hash_get(manifest->hosts, w->peer);
	pthread_mutex_unlock(&manifest_mutex);

	w->policy = NULL;
	return 0;
}

static int handle_hello(worker *w)
{
	if (w->peer_verified) {
		pdu_send_HELLO(&w->session);
	} else {
		pdu_send_ERROR(&w->session, 401, "Peer Certificate Required");
	}
	return 1;
}

static int handle_get_cert(worker *w)
{
	char *csr_file, *cert_file;
	X509_REQ *csr = X509_REQ_new();
	X509 *cert = NULL;

	if (pdu_decode_GET_CERT(RECV_PDU(&w->session), &csr) != 0) {
		CRITICAL("Unable to decode GET_CERT PDU");
		return 0;
	}

	/* FIXME: how do we verify CSR integrity? */

	csr_file  = string("%s/%s.csr", w->requests_dir, w->peer);
	cert_store_request(csr, csr_file);
	free(csr_file);

	cert_file = string("%s/%s.pem", w->certs_dir, w->peer);
	cert = cert_retrieve_certificate(cert_file);
	free(cert_file);

	if (pdu_send_SEND_CERT(&w->session, cert) < 0) { return 0; }

	return 1;
}

static int handle_get_policy(worker *w)
{
	if (!w->peer_verified) {
		WARNING("Unverified peer tried to FACTS");
		pdu_send_ERROR(&w->session, 401, "Peer Certificate Required");
		return 1;
	}

	if (pdu_decode_FACTS(RECV_PDU(&w->session), w->facts) != 0) {
		CRITICAL("Unable to decode FACTS");
		return 0;
	}

	if (!w->policy && w->pnode) {
		w->policy = policy_generate(w->pnode, w->facts);
	}

	if (!w->policy) {
		CRITICAL("Unable to generate policy for host %s", w->peer);
		return 0;
	}

	if (pdu_send_POLICY(&w->session, w->policy) < 0) { return 0; }

	return 1;
}

static int handle_get_file(worker *w)
{
	char hex[SHA1_HEX_DIGEST_SIZE + 1] = {0};
	sha1 checksum;
	struct res_file *file, *match;

	if (!w->peer_verified) {
		WARNING("Unverified peer tried to FILE");
		pdu_send_ERROR(&w->session, 401, "Peer Certificate Required");
		return 1;
	}

	if (RECV_PDU(&w->session)->len != SHA1_HEX_DIGEST_SIZE) {
		pdu_send_ERROR(&w->session, 400, "Malformed FILE request");
		return 1;
	}

	memcpy(hex, RECV_PDU(&w->session)->data, SHA1_HEX_DIGEST_SIZE);
	sha1_init(&checksum, hex);

	/* Search for the res_file in the policy */
	if (!w->policy) {
		WARNING("FILE before FACTS");
		pdu_send_ERROR(&w->session, 405, "Called FILE before FACTS");
		return 0;
	}

	DEBUG("FILE: %s", checksum.hex);

	file = NULL;
	for_each_node(match, &w->policy->res_files, res) {
		DEBUG("Check '%s' against '%s'", match->rf_rsha1.hex, checksum.hex);
		if (sha1_cmp(&match->rf_rsha1, &checksum) == 0) {
			file = match;
			break;
		}
	}

	if (file) {
		DEBUG("Found a res_file for %s: %s\n", checksum.hex, file->rf_rpath);
		if (worker_send_file(w, file->rf_rpath) != 0) {
			DEBUG("Unable to send file");
		}
	} else {
		pdu_send_ERROR(&w->session, 404, "File Not Found");
		DEBUG("Could not find a res_file for: %s\n", checksum.hex);
	}

	return 1;
}

static int handle_put_report(worker *w)
{
	if (!w->peer_verified) {
		WARNING("Unverified peer tried to REPORT");
		pdu_send_ERROR(&w->session, 401, "Peer Certificate Required");
		return 1;
	}

	/* FIXME: process the SEND_REPORT PDU(s) */
	pdu_send_BYE(&w->session);

	return 1;
}

static int handle_unknown(worker *w)
{
	char *message;
	protocol_op op = RECV_PDU(&w->session)->op;

	message = string("Protocol Error: Unrecognized PDU op %s(%u)", protocol_op_name(op), op);
	WARNING("%s", message);
	pdu_send_ERROR(&w->session, 405, message);
	free(message);

	return 1;
}

static int worker_dispatch(worker *w)
{
	assert(w);

	for (;;) {
		pdu_receive(&w->session);
		switch (RECV_PDU(&w->session)->op) {

		case PROTOCOL_OP_HELLO:
			if (!handle_hello(w)) { return -2; }
			break;

		case PROTOCOL_OP_BYE:
			return 0;

		case PROTOCOL_OP_GET_CERT:
			if (!handle_get_cert(w)) { return -2; }
			break;

		case PROTOCOL_OP_FACTS:
			if (!handle_get_policy(w)) { return -2; }
			break;

		case PROTOCOL_OP_FILE:
			if (!handle_get_file(w)) { return -2; }
			break;

		case PROTOCOL_OP_REPORT:
			if (!handle_put_report(w)) { return -2; }
			break;

		default:
			handle_unknown(w);
			return -1;
		}
	}
}

static void worker_die(worker *w)
{
	assert(w);

	SSL_shutdown(w->ssl);
	SSL_free(w->ssl);
	ERR_remove_state(0);

	free(w->peer);
	free(w);
}

static int worker_verify_peer(worker *w)
{
	assert(w);

	long err;
	int sock;
	char addr[256];

	sock = SSL_get_fd(w->ssl);
	if (protocol_reverse_lookup_verify(sock, addr, 256) != 0) {
		ERROR("FCrDNS lookup (ipv4) failed: %s", strerror(errno));
		return -1;
	}
	w->peer = strdup(addr);
	INFO("Connection on socket %u from '%s'", sock, w->peer);

	w->peer_verified = 0;

	/* FIXME: this is a bit of a mess... */
	if ((err = protocol_ssl_verify_peer(w->ssl, addr)) != X509_V_OK) {
		if (err != X509_V_ERR_APPLICATION_VERIFICATION) {
			ERROR("SSL: problem with peer certificate: %s", X509_verify_cert_error_string(err));
			protocol_ssl_backtrace();
		}
	} else {
		w->peer_verified = 1;
	}

	return 0;
}

static int worker_send_file(worker *w, const char *path)
{
	int fd = open(path, O_RDONLY);
	int n;
	while ((n = pdu_send_DATA(&w->session, fd)) > 0)
		;

	return n;
}

static void server_sighup(int signum, siginfo_t *info, void *udata)
{
	pthread_t tid;
	void *status;

	DEBUG("SIGHUP handler entered");

	pthread_create(&tid, NULL, server_manager_thread, NULL);
	pthread_join(tid, &status);
}

/**
  Daemonize the current process, by forking into the background,
  closing stdin, stdout and stderr, and detaching from the
  controlling terminal.

  The \a name, \a lock_file, and \a pid_file parameters allow
  the caller to control how the process is daemonized.

  @param  lock_file  Path to use as a file lock, to prevent multiple
                     daemon instances from runnin concurrently.
  @param  pid_file   Path to store the daemon's ultimate process ID.

  @returns to the caller if daemonization succeeded.  The parent
           process exits with a status code of 0.  If daemonization
           fails, this call will not return, and a the current
           process will exit non-zero.
 */
static void daemonize(const char *lock_file, const char *pid_file)
{
	pid_t pid, sessid;
	int lock_fd;
	FILE *pid_io;

	/* are we already a child of init? */
	if (getppid() == 1) { return; }

	/* first fork */
	pid = fork();
	if (pid < 0) {
		ERROR("unable to fork: %s", strerror(errno));
		exit(2);
	}
	if (pid > 0) { /* parent process */
		_exit(0);
	}

	/* second fork */
	sessid = setsid(); /* new process session / group */
	if (sessid < 0) {
		ERROR("unable to create new process group: %s", strerror(errno));
		exit(2);
	}

	pid = fork();
	if (pid < 0) {
		ERROR("unable to fork again: %s", strerror(errno));
		exit(2);
	}

	if (pid > 0) { /* "middle" child / parent */
		/* write pid to file
		     Now is the only time we have access to the PID of
		     the fully daemonized process. */
		pid_io = fopen(pid_file, "w");
		if (!pid_io) {
			ERROR("Failed to open PID file %s for writing: %s", pid_file, strerror(errno));
			exit(2);
		}
		fprintf(pid_io, "%lu\n", (unsigned long)pid);
		fclose(pid_io);
		_exit(0);
	}

	/* acquire lock */
	if (!(lock_file && lock_file[0])) {
		ERROR("NULL or empty lock file path given");
		exit(2);
	}

	lock_fd = open(lock_file, O_CREAT | O_RDWR, 0640);
	if (lock_fd < 0) {
		ERROR("Failed to open lock file %s: %s", lock_file, strerror(errno));
		exit(2);
	}

	if (lockf(lock_fd, F_TLOCK, 0) < 0) {
		ERROR("Failed to lock %s; daemon already running?", lock_file);
		exit(2);
	}

	/* settle */
	umask(0777); /* reset the file umask */
	if (chdir("/") < 0) {
		ERROR("unable to chdir to /: %s", strerror(errno));
		exit(2);
	}

	/* redirect standard fds */
	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);
}

static int server_init(server *s)
{
	assert(s);

	struct sigaction sig;

	if (manifest) {
		ERROR("server module already initialized");
		return -1;
	}

	pthread_mutex_lock(&manifest_mutex);
		manifest_file = s->manifest_file;
		manifest = parse_file(manifest_file);
	pthread_mutex_unlock(&manifest_mutex);

	if (server_init_ssl(s) != 0) { return -1; }

	/* sig handlers */
	INFO("setting up signal handlers");
	sig.sa_sigaction = server_sighup;
	sig.sa_flags = SA_SIGINFO;
	sigemptyset(&sig.sa_mask);
	if (sigaction(SIGHUP, &sig, NULL) != 0) {
		ERROR("Unable to set signal handlers: %s", strerror(errno));
		/* FIXME: cleanup */
		return -1;
	}

	/* daemonize, if necessary */
	if (s->daemonize == SERVER_OPT_TRUE) {
		INFO("daemonizing");
		daemonize(s->lock_file, s->pid_file);
		log_init("policyd");
	} else {
		INFO("running in foreground");
	}

	log_level(s->log_level);

	/* bind socket */
	INFO("binding SSL socket on port %s", s->port);
	s->listener = BIO_new_accept(strdup(s->port));
	if (!s->listener || BIO_do_accept(s->listener) <= 0) {
		return -1;
	}

	return 0;
}

static void* server_worker_thread(void *arg)
{
	worker *w = (worker*)arg;

	pthread_detach(pthread_self());

	if (worker_prep(w) != 0) {
		worker_die(w);
		return NULL;
	}

	worker_dispatch(w);
	worker_die(w);

	INFO("SSL connection closed");
	return NULL;
}

static void* server_manager_thread(void *arg)
{
	struct manifest *new_manifest;

	new_manifest = parse_file(manifest_file);
	if (new_manifest) {
		INFO("Updating server manifest");
		pthread_mutex_lock(&manifest_mutex);
		manifest = new_manifest;
		pthread_mutex_unlock(&manifest_mutex);
	} else {
		WARNING("Unable to parse manifest; skipping");
	}

	return NULL;
}

static int server_init_ssl(server *s)
{
	assert(s->ca_cert_file);
	assert(s->cert_file);
	assert(s->key_file);

	protocol_ssl_init();

	INFO("Setting up server SSL context");
	if (!(s->ssl_ctx = SSL_CTX_new(TLSv1_method()))) {
		ERROR("Failed to set up new TLSv1 SSL context");
		protocol_ssl_backtrace();
		return -1;
	}

	if (!SSL_CTX_load_verify_locations(s->ssl_ctx, s->ca_cert_file, NULL)) {
		ERROR("Failed to load CA certificate chain (%s)", s->ca_cert_file);
		goto error;
	}
	if (!SSL_CTX_use_certificate_file(s->ssl_ctx, s->cert_file, SSL_FILETYPE_PEM)) {
		ERROR("Failed to load certificate from file (%s)", s->cert_file);
		goto error;
	}
	if (!SSL_CTX_use_PrivateKey_file(s->ssl_ctx, s->key_file, SSL_FILETYPE_PEM)) {
		ERROR("Failed to load private key (%s)", s->key_file);
		goto error;
	}

	SSL_CTX_set_verify(s->ssl_ctx, SSL_VERIFY_PEER, NULL);
	SSL_CTX_set_verify_depth(s->ssl_ctx, 4);
	return 0;

error:
	SSL_CTX_free(s->ssl_ctx);
	s->ssl_ctx = NULL;
	protocol_ssl_backtrace();
	return -1;
}

