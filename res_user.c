#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "res_user.h"
#include "mem.h"

static int _res_user_diff(struct res_user *ru);

/*****************************************************************/

static int _res_user_diff(struct res_user *ru)
{
	assert(ru);

	ru->ru_diff = RES_USER_NONE;

	if (res_user_enforced(ru, NAME) && strcmp(ru->ru_name, ru->ru_pw.pw_name) != 0) {
		ru->ru_diff |= RES_USER_NAME;
	}

	if (res_user_enforced(ru, PASSWD) && strcmp(ru->ru_passwd, ru->ru_pw.pw_passwd) != 0) {
		ru->ru_diff |= RES_USER_PASSWD;
	}

	if (res_user_enforced(ru, UID) && ru->ru_uid != ru->ru_pw.pw_uid) {
		ru->ru_diff |= RES_USER_UID;
	}

	if (res_user_enforced(ru, GID) && ru->ru_gid != ru->ru_pw.pw_gid) {
		ru->ru_diff |= RES_USER_GID;
	}

	if (res_user_enforced(ru, GECOS) && strcmp(ru->ru_gecos, ru->ru_pw.pw_gecos) != 0) {
		ru->ru_diff |= RES_USER_GECOS;
	}

	if (res_user_enforced(ru, DIR) && strcmp(ru->ru_dir, ru->ru_pw.pw_dir) != 0) {
		ru->ru_diff |= RES_USER_DIR;
	}

	if (res_user_enforced(ru, SHELL) && strcmp(ru->ru_shell, ru->ru_pw.pw_shell) != 0) {
		ru->ru_diff |= RES_USER_SHELL;
	}

	return 0;
}

/*****************************************************************/

void res_user_init(struct res_user *ru)
{
	assert(ru);

	ru->ru_prio = 0;

	ru->ru_name = NULL;
	ru->ru_passwd = NULL;

	ru->ru_uid = 0;
	ru->ru_uid = 0;

	ru->ru_gecos = NULL;
	ru->ru_dir = NULL;
	ru->ru_shell = NULL;

	memset(&ru->ru_pw, 0, sizeof(struct passwd));

	ru->ru_enf = RES_USER_NONE;
	ru->ru_diff = RES_USER_NONE;
}

void res_user_free(struct res_user *ru)
{
	assert(ru);

	xfree(ru->ru_name);
	xfree(ru->ru_passwd);
	xfree(ru->ru_gecos);
	xfree(ru->ru_dir);
	xfree(ru->ru_shell);
}

int res_user_set_name(struct res_user *ru, const char *name)
{
	assert(ru);

	xfree(ru->ru_name);
	ru->ru_name = strdup(name);
	if (!ru->ru_name) { return -1; }

	ru->ru_enf |= RES_USER_NAME;
	return 0;
}

int res_user_unset_name(struct res_user *ru)
{
	assert(ru);

	ru->ru_enf ^= RES_USER_NAME;
	return 0;
}

int res_user_set_passwd(struct res_user *ru, const char *passwd)
{
	assert(ru);

	xfree(ru->ru_passwd);
	ru->ru_passwd = strdup(passwd);
	if (!ru->ru_passwd) { return -1; }

	ru->ru_enf |= RES_USER_PASSWD;
	return 0;
}

int res_user_unset_passwd(struct res_user *ru)
{
	assert(ru);

	ru->ru_enf ^= RES_USER_PASSWD;
	return 0;
}

int res_user_set_uid(struct res_user *ru, uid_t uid)
{
	assert(ru);

	ru->ru_uid = uid;

	ru->ru_enf |= RES_USER_UID;
	return 0;
}

int res_user_unset_uid(struct res_user *ru)
{
	assert(ru);

	ru->ru_enf ^= RES_USER_UID;
	return 0;
}

int res_user_set_gid(struct res_user *ru, gid_t gid)
{
	assert(ru);

	ru->ru_gid = gid;

	ru->ru_enf |= RES_USER_GID;
	return 0;
}

int res_user_unset_gid(struct res_user *ru)
{
	assert(ru);

	ru->ru_enf ^= RES_USER_GID;
	return 0;
}

int res_user_set_gecos(struct res_user *ru, const char *gecos)
{
	assert(ru);

	xfree(ru->ru_gecos);
	ru->ru_gecos = strdup(gecos);
	if (!ru->ru_gecos) { return -1; }

	ru->ru_enf |= RES_USER_GECOS;
	return 0;
}

int res_user_unset_gecos(struct res_user *ru)
{
	assert(ru);

	ru->ru_enf ^= RES_USER_GECOS;
	return 0;
}

int res_user_set_dir(struct res_user *ru, const char *path)
{
	assert(ru);

	xfree(ru->ru_dir);
	ru->ru_dir = strdup(path);
	if (!ru->ru_dir) { return -1; }

	ru->ru_enf |= RES_USER_DIR;
	return 0;
}

int res_user_unset_dir(struct res_user *ru)
{
	assert(ru);

	ru->ru_enf ^= RES_USER_DIR;
	return 0;
}

int res_user_set_shell(struct res_user *ru, const char *shell)
{
	assert(ru);

	xfree(ru->ru_shell);
	ru->ru_shell = strdup(shell);
	if (!ru->ru_shell) { return -1; }

	ru->ru_enf |= RES_USER_SHELL;
	return 0;
}

int res_user_unset_shell(struct res_user *ru)
{
	assert(ru);

	ru->ru_enf ^= RES_USER_SHELL;
	return 0;
}

void res_user_merge(struct res_user *ru1, struct res_user *ru2)
{
	assert(ru1);
	assert(ru2);

	struct res_user *tmp;

	if (ru1->ru_prio > ru2->ru_prio) {
		/* out-of-order, swap pointers */
		tmp = ru1; ru1 = ru2; ru2 = ru1; tmp = NULL;
	}

	/* ru1 as priority over ru2 */
	assert(ru1->ru_prio <= ru2->ru_prio);

	if ( res_user_enforced(ru2, NAME) &&
	    !res_user_enforced(ru1, NAME)) {
		printf("Overriding NAME of ru1 with value from ru2\n");
		res_user_set_name(ru1, ru2->ru_name);
	}

	if ( res_user_enforced(ru2, PASSWD) &&
	    !res_user_enforced(ru1, PASSWD)) {
		printf("Overriding PASSWD of ru1 with value from ru2\n");
		res_user_set_passwd(ru1, ru2->ru_passwd);
	}

	if ( res_user_enforced(ru2, UID) &&
	    !res_user_enforced(ru1, UID)) {
		printf("Overriding UID of ru1 with value from ru2\n");
		res_user_set_uid(ru1, ru2->ru_uid);
	}

	if ( res_user_enforced(ru2, GID) &&
	    !res_user_enforced(ru1, GID)) {
		printf("Overriding GID of ru1 with value from ru2\n");
		res_user_set_gid(ru1, ru2->ru_gid);
	}

	if ( res_user_enforced(ru2, GECOS) &&
	    !res_user_enforced(ru1, GECOS)) {
		printf("Overriding GECOS of ru1 with value from ru2\n");
		res_user_set_gecos(ru1, ru2->ru_gecos);
	}

	if ( res_user_enforced(ru2, DIR) &&
	    !res_user_enforced(ru1, DIR)) {
		printf("Overriding DIR of ru1 with value from ru2\n");
		res_user_set_dir(ru1, ru2->ru_dir);
	}

	if ( res_user_enforced(ru2, SHELL) &&
	    !res_user_enforced(ru1, SHELL)) {
		printf("Overriding SHELL of ru1 with value from ru2\n");
		res_user_set_shell(ru1, ru2->ru_shell);
	}
}

int res_user_stat(struct res_user *ru)
{
	assert(ru);

	struct passwd *entry = NULL;

	/* FIXME: how to deal with diff UID and name? */
	/* for now, prefer UID match over name match */

	/* getpuid and getpwnam return NULL on error OR no match.
	   clear errno manually to test for errors. */
	errno = 0;

	if (res_user_enforced(ru, UID)) {
		printf("Looking for user by UID (%u)\n", ru->ru_uid);
		entry = getpwuid(ru->ru_uid);
		if (!entry && errno) { return -1; }
	}

	if (!entry && res_user_enforced(ru, NAME)) {
		printf("Looking for user by name (%s)\n", ru->ru_name);
		entry = getpwnam(ru->ru_name);
		if (!entry && errno) { return -1; }
	}

	if (entry) {
		/* entry may point to static storage cf. getpwnam(3); */
		memcpy(&ru->ru_pw, entry, sizeof(struct passwd));
	}

	return _res_user_diff(ru);
}

void res_user_dump(struct res_user *ru)
{
	printf("\n\n");
	printf("struct res_user (0x%0x) {\n", (unsigned int)ru);
	printf("    ru_name: \"%s\"\n", ru->ru_name);
	printf("  ru_passwd: \"%s\"\n", ru->ru_passwd);
	printf("     ru_uid: %u\n", ru->ru_uid);
	printf("     ru_gid: %u\n", ru->ru_gid);
	printf("   ru_gecos: \"%s\"\n", ru->ru_gecos);
	printf("     ru_dir: \"%s\"\n", ru->ru_dir);
	printf("   ru_shell: \"%s\"\n", ru->ru_shell);
	printf("--- (ru_pw omitted) ---\n");

	printf("      ru_pw: struct passwd {\n");
	printf("                 pw_name: \"%s\"\n", ru->ru_pw.pw_name);
	printf("               pw_passwd: \"%s\"\n", ru->ru_pw.pw_passwd);
	printf("                  pw_uid: %u\n", ru->ru_pw.pw_uid);
	printf("                  pw_gid: %u\n", ru->ru_pw.pw_gid);
	printf("                pw_gecos: \"%s\"\n", ru->ru_pw.pw_gecos);
	printf("                  pw_dir: \"%s\"\n", ru->ru_pw.pw_dir);
	printf("                pw_shell: \"%s\"\n", ru->ru_pw.pw_shell);
	printf("             }\n");

	printf("     ru_enf: %o\n", ru->ru_enf);
	printf("    ru_diff: %o\n", ru->ru_diff);
	printf("}\n");
	printf("\n");

	printf("NAME:   %s (%02o & %02o == %02o)\n",
	       (res_user_enforced(ru, NAME) ? "enforced  " : "unenforced"),
	       ru->ru_enf, RES_USER_NAME, ru->ru_enf & RES_USER_NAME);
	printf("PASSWD: %s (%02o & %02o == %02o)\n",
	       (res_user_enforced(ru, PASSWD) ? "enforced  " : "unenforced"),
	       ru->ru_enf, RES_USER_PASSWD, ru->ru_enf & RES_USER_PASSWD);
	printf("UID:    %s (%02o & %02o == %02o)\n",
	       (res_user_enforced(ru, UID) ? "enforced  " : "unenforced"),
	       ru->ru_enf, RES_USER_UID, ru->ru_enf & RES_USER_UID);
	printf("GID:    %s (%02o & %02o == %02o)\n",
	       (res_user_enforced(ru, GID) ? "enforced  " : "unenforced"),
	       ru->ru_enf, RES_USER_GID, ru->ru_enf & RES_USER_GID);
	printf("GECOS:  %s (%02o & %02o == %02o)\n",
	       (res_user_enforced(ru, GECOS) ? "enforced  " : "unenforced"),
	       ru->ru_enf, RES_USER_GECOS, ru->ru_enf & RES_USER_GECOS);
	printf("DIR:    %s (%02o & %02o == %02o)\n",
	       (res_user_enforced(ru, DIR) ? "enforced  " : "unenforced"),
	       ru->ru_enf, RES_USER_DIR, ru->ru_enf & RES_USER_DIR);
	printf("SHELL:  %s (%02o & %02o == %02o)\n",
	       (res_user_enforced(ru, SHELL) ? "enforced  " : "unenforced"),
	       ru->ru_enf, RES_USER_SHELL, ru->ru_enf & RES_USER_SHELL);
}
