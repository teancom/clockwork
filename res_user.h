#ifndef RES_USER_H
#define RES_USER_H

#include <stdlib.h>
#include <sys/types.h>
#include <sys/types.h>
#include <pwd.h>

#define RES_USER_NONE   0000
#define RES_USER_NAME   0001
#define RES_USER_PASSWD 0002
#define RES_USER_UID    0004
#define RES_USER_GID    0010
#define RES_USER_GECOS  0020
#define RES_USER_DIR    0040
#define RES_USER_SHELL  0100

#define res_user_enforced(ru, flag)  (((ru)->ru_enf  & RES_USER_ ## flag) == RES_USER_ ## flag)
#define res_user_different(ru, flag) (((ru)->ru_diff & RES_USER_ ## flag) == RES_USER_ ## flag)

struct res_user {
	char          *ru_name;    /* Username */
	char          *ru_passwd;  /* Encrypted Password */

	uid_t          ru_uid;     /* Numeric User ID */
	gid_t          ru_gid;     /* Numeric Group ID (primary) */

	char          *ru_gecos;   /* Comment (GECOS) field */
	char          *ru_dir;     /* Home directory */
	char          *ru_shell;   /* Path to shell */

	struct passwd *ru_pw;      /* getpwnam(3) / getpwuid(3) of user */

	unsigned int   ru_prio;
	unsigned int   ru_enf;     /* enforce-compliance flags */
	unsigned int   ru_diff;    /* out-of-compliance flags */
};

void res_user_init(struct res_user *ru);
void res_user_free(struct res_user *ru);

int res_user_set_name(struct res_user *ru, const char *name);
int res_user_unset_name(struct res_user *ru);

int res_user_set_passwd(struct res_user *ru, const char *passwd);
int res_user_unset_passwd(struct res_user *ru);

int res_user_set_uid(struct res_user *ru, uid_t uid);
int res_user_unset_uid(struct res_user *ru);

int res_user_set_gid(struct res_user *ru, gid_t gid);
int res_user_unset_gid(struct res_user *ru);

int res_user_set_gecos(struct res_user *ru, const char *gecos);
int res_user_unset_gecos(struct res_user *ru);

int res_user_set_dir(struct res_user *ru, const char *path);
int res_user_unset_dir(struct res_user *ru);

int res_user_set_shell(struct res_user *ru, const char *shell);
int res_user_unset_shell(struct res_user *ru);

void res_user_merge(struct res_user *ru1, struct res_user *ru2);

int res_user_stat(struct res_user *ru);
void res_user_dump(struct res_user *ru);

#endif
