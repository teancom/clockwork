#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "../userdb.h"

/*********************************************************/

#define ABORT_CODE 42

/* for pwdb_init failure-to-open scenarios */
#define BAD_FILE "/non/existent/file"
#define NOT_DIR  "/etc/passwd/is/not/a/directory"

/* test /etc/passwd file */
#define PWFILE     "test/data/passwd"
#define PWFILE_NEW "test/tmp/passwd.new"

/* test /etc/shadow file */
#define SPFILE     "test/data/shadow"
#define SPFILE_NEW "test/tmp/shadow.new"

/*********************************************************/

static struct pwdb *open_passwd(const char *path)
{
	struct pwdb *db;
	char buf[256];

	db = pwdb_init(path);
	if (!db) {
		if (snprintf(buf, 256, "Unable to open %s", path) < 0) {
			perror("open_passwd / snprintf failed");
		} else {
			perror(buf);
		}
		exit(ABORT_CODE);
	}

	return db;
}

static struct spdb *open_shadow(const char *path)
{
	struct spdb *db;
	char buf[256];

	db = spdb_init(path);
	if (!db) {
		if (snprintf(buf, 256, "Unable to open %s", path) < 0) {
			perror("open_shadow / snprintf failed");
		} else {
			perror(buf);
		}
		exit(ABORT_CODE);
	}

	return db;
}

/*********************************************************/

static void assert_passwd(struct passwd *pw, const char *name, uid_t uid, gid_t gid)
{
	char buf[256];

	snprintf(buf, 256, "%s UID should be %u", name, uid);
	assert_int_equals(buf, pw->pw_uid, uid);

	snprintf(buf, 256, "%s GID should be %u", name, gid);
	assert_int_equals(buf, pw->pw_gid, gid);

	snprintf(buf, 256, "%s username should be %s", name, name);
	assert_str_equals(buf, pw->pw_name, name);
}

static void assert_pwdb_get(struct pwdb *db, const char *name, uid_t uid, gid_t gid)
{
	char buf[256];
	struct passwd *pw;

	pw = pwdb_get_by_name(db, name);

	snprintf(buf, 256, "Look up %s user by name", name);
	assert_not_null(buf, pw);
	assert_passwd(pw, name, uid, gid);

	pw = pwdb_get_by_uid(db, uid);

	snprintf(buf, 256, "Look up %s user by UID (%u)", name, uid);
	assert_not_null(buf, pw);
	assert_passwd(pw, name, uid, gid);
}

static void assert_spdb_get(struct spdb *db, const char *name)
{
	char buf[256];
	struct spwd *sp;

	sp = spdb_get_by_name(db, name);

	snprintf(buf, 256, "Look up %s user by name", name);
	assert_not_null(buf, sp);

	snprintf(buf, 256, "%s username should be %s", name, name);
	assert_str_equals(buf, sp->sp_namp, name);
}

void test_pwdb_init()
{
	struct pwdb *db;

	test("PWDB: Initialize passwd database structure");

	assert_null("fail to open non-existent file (" BAD_FILE ")", pwdb_init(BAD_FILE));
	assert_null("fail to open bad path (" NOT_DIR ")", pwdb_init(NOT_DIR));
	assert_not_null("open " PWFILE, db = pwdb_init(PWFILE));

	pwdb_free(db);
}

void test_pwdb_get()
{
	struct pwdb *db;
	struct passwd *pw;

	db = open_passwd(PWFILE);

	test("PWDB: Lookup of root (1st passwd entry)");
	assert_pwdb_get(db, "root", 0, 0);

	test("PWDB: Lookup of svc (last passwd entry)");
	assert_pwdb_get(db, "svc", 999, 909);

	test("PWDB: Lookup of user (middle of passwd database)");
	assert_pwdb_get(db, "user", 100, 20);

	pwdb_free(db);
}

void test_pwdb_add()
{
	struct pwdb *db;
	struct passwd pw, *ent;

	db = open_passwd(PWFILE);

	memset(&pw, 0, sizeof(pw));
	test("PWBD: Add an entry to passwd database");
	pw.pw_name = "new_user";
	pw.pw_passwd = "x";
	pw.pw_uid = 500;
	pw.pw_gid = 500;
	pw.pw_gecos = "New User,,,";
	pw.pw_dir = "/home/new_user";
	pw.pw_shell = "/bin/bash";

	assert_null("new_user account doesn't already exist", pwdb_get_by_name(db, pw.pw_name));
	assert_true("creation of new user account", pwdb_add(db, &pw) == 0);
	assert_not_null("new entry exists in memory", pwdb_get_by_name(db, pw.pw_name));
	assert_true("save passwd database to " PWFILE_NEW, pwdb_write(db, PWFILE_NEW) == 0);

	pwdb_free(db);
	db = open_passwd(PWFILE_NEW);
	assert_not_null("re-read new entry from " PWFILE_NEW, ent = pwdb_get_by_name(db, pw.pw_name));

	assert_str_equals("  check equality of pw_name",   "new_user",       ent->pw_name);
	assert_str_equals("  check equality of pw_passwd", "x",              ent->pw_passwd);
	assert_int_equals("  check equality of pw_uid",    500,              ent->pw_uid);
	assert_int_equals("  check equality of pw_gid",    500,              ent->pw_gid);
	assert_str_equals("  check equality of pw_gecos",  "New User,,,",    ent->pw_gecos);
	assert_str_equals("  check equality of pw_dir",    "/home/new_user", ent->pw_dir);
	assert_str_equals("  check equality of pw_shell",  "/bin/bash",      ent->pw_shell);

	if (unlink(PWFILE_NEW) == -1) {
		perror("Unable to remove " PWFILE_NEW);
		exit(ABORT_CODE);
	}

	pwdb_free(db);
}

void test_pwdb_rm()
{
	struct pwdb *db;
	struct passwd *pw;
	const char *rm_user = "sys";

	db = open_passwd(PWFILE);

	test("PWDB: Remove an entry from passwd database");
	assert_not_null("account to be removed exists", pw = pwdb_get_by_name(db, rm_user));
	assert_true("removal of account", pwdb_rm(db, pw) == 0);
	assert_null("removed entry does not exist in memory", pwdb_get_by_name(db, rm_user));
	assert_true("save passwd database to " PWFILE_NEW, pwdb_write(db, PWFILE_NEW) == 0);

	pwdb_free(db);
	db = open_passwd(PWFILE_NEW);

	assert_null("removed entry does not exist in " PWFILE_NEW, pwdb_get_by_name(db, rm_user));

	pwdb_free(db);
}

void test_pwdb_rm_head()
{
	struct pwdb *db;
	struct passwd *pw;
	char *name;

	db = open_passwd(PWFILE);

	test("PWDB: Removal of list head (first entry)");

	pw = db->passwd;
	name = strdup(pw->pw_name);

	assert_true("removal of first account in list", pwdb_rm(db, pw) == 0);
	assert_null("removed entry does not exist in memory", pwdb_get_by_name(db, name));
	assert_true("save passwd database to " PWFILE_NEW, pwdb_write(db, PWFILE_NEW) == 0);

	pwdb_free(db);
	db = open_passwd(PWFILE_NEW);

	assert_null("removed entry does not exist in " PWFILE_NEW, pwdb_get_by_name(db, name));

	free(name);
	pwdb_free(db);
}

void test_spdb_init()
{
	struct spdb *db;

	test("SPDB: Initialize passwd database structure");
	assert_null("fail to open non-existent file (" BAD_FILE ")", spdb_init(BAD_FILE));
	assert_null("fail to open bad path (" NOT_DIR ")", spdb_init(NOT_DIR));
	assert_not_null("open " SPFILE, db = spdb_init(SPFILE));

	spdb_free(db);
}

void test_spdb_get()
{
	struct spdb *db;
	struct spwd *sp;

	db = open_shadow(SPFILE);

	test("SPDB: Lookup of root (1st shadow entry)");
	assert_spdb_get(db, "root");

	test("SPDB: Lookup of svc (last shadow entry)");
	assert_spdb_get(db, "svc");

	test("SPDB: Lookup of user (middle of shadow database)");
	assert_spdb_get(db, "user");

	spdb_free(db);
}

void test_spdb_add()
{
	struct spdb *db;
	struct spwd sp, *ent;

	db = open_shadow(SPFILE);

	memset(&sp, 0, sizeof(sp));
	test("SPDB: Add an entry to shadow database");
	sp.sp_namp = "new_user";
	sp.sp_pwdp = "$6$pwhash";
	sp.sp_lstchg = 14800;
	sp.sp_min = 1;
	sp.sp_max = 6;
	sp.sp_warn = 4;
	sp.sp_inact = 0;
	sp.sp_expire = 0;

	assert_null("new_user entry doesn't already exist", spdb_get_by_name(db, sp.sp_namp));
	assert_true("creation of new shadow entry", spdb_add(db, &sp) == 0);
	assert_not_null("new entry exists in memory", spdb_get_by_name(db, sp.sp_namp));
	assert_true("save shadow database to " SPFILE_NEW, spdb_write(db, SPFILE_NEW) == 0);

	spdb_free(db);
	db = open_shadow(SPFILE_NEW);

	assert_not_null("re-read net entry from " SPFILE_NEW, ent = spdb_get_by_name(db, sp.sp_namp));
	assert_str_equals("  check equality of sp_namp",   "new_user",  ent->sp_namp);
	assert_str_equals("  check equality of sp_pwdp",   "$6$pwhash", ent->sp_pwdp);
	assert_int_equals("  check equality of sp_lstchg", 14800,       ent->sp_lstchg);
	assert_int_equals("  check equality of sp_min",    1,           ent->sp_min);
	assert_int_equals("  check equality of sp_max",    6,           ent->sp_max);
	assert_int_equals("  check equality of sp_warn",   4,           ent->sp_warn);
	assert_int_equals("  check equality of sp_inact",  0,           ent->sp_inact);
	assert_int_equals("  check equality of sp_expire", 0,           ent->sp_expire);

	if (unlink(SPFILE_NEW) == -1) {
		perror("Unable to remove " SPFILE_NEW);
		exit(ABORT_CODE);
	}

	spdb_free(db);
}

void test_spdb_rm()
{
	struct spdb *db;
	struct spwd *sp;
	const char *rm_user = "sys";

	db = open_shadow(SPFILE);

	test("SPDB: Remove an entry from shadow database");
	assert_not_null("account to be removed exists", sp = spdb_get_by_name(db, rm_user));
	assert_true("removal of account", spdb_rm(db, sp) == 0);
	assert_null("removed entry does not exist in memory", spdb_get_by_name(db, rm_user));
	assert_true("save shadow database to " SPFILE_NEW, spdb_write(db, SPFILE_NEW) == 0);

	spdb_free(db);
	db = open_shadow(SPFILE_NEW);

	assert_null("removed entry does not exist in " PWFILE_NEW, spdb_get_by_name(db, rm_user));

	spdb_free(db);
}

void test_spdb_rm_head()
{
	struct spdb *db;
	struct spwd *sp;
	char *name;

	db = open_shadow(SPFILE);

	test("SPDB: Removal of list head (first entry)");

	sp = db->spwd;
	name = strdup(sp->sp_namp);

	assert_true("removal of first account in list", spdb_rm(db, sp) == 0);
	assert_null("removed entry does not exist in memory", spdb_get_by_name(db, name));
	assert_true("save passwd database to " SPFILE_NEW, spdb_write(db, SPFILE_NEW) == 0);

	spdb_free(db);
	db = open_shadow(SPFILE_NEW);

	assert_null("removed entry does not exist in " SPFILE_NEW, spdb_get_by_name(db, name));

	free(name);
	spdb_free(db);
}

void test_suite_userdb() {

	/* passwd db tests */
	test_pwdb_init();
	test_pwdb_get();
	test_pwdb_add();
	test_pwdb_rm();
	test_pwdb_rm_head();

	/* shadow db tests */
	test_spdb_init();
	test_spdb_get();
	test_spdb_add();
	test_spdb_rm();
	test_spdb_rm_head();
}