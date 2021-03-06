#!/usr/bin/perl
use strict;
use warnings;

use Test::More;
use t::common;

gencode_ok "use host dir1.test", <<'EOF', "dir resource";
RESET
TOPIC "dir(/etc/sudoers)"
SET %A "/etc/sudoers"
CALL &FS.EXISTS?
OK? @exists.1
  CALL &FS.MKDIR
  OK? @exists.1
  ERROR "Failed to create new directory '%s'"
  JUMP @next.1
exists.1:
CALL &FS.IS_DIR?
OK? @isdir.1
  ERROR "%s exists, but is not a directory"
  JUMP @next.1
isdir.1:
COPY %A %F
SET %D 0
SET %E 0
CALL &USERDB.OPEN
OK? @owner.lookup.1
  ERROR "Failed to open the user database"
  HALT
owner.lookup.1:
SET %A 1
SET %B "root"
CALL &USER.FIND
OK? @found.user.1
  COPY %B %A
  ERROR "Failed to find user '%s'"
  JUMP @next.1
found.user.1:
CALL &USER.GET_UID
COPY %R %D
SET %A 1
SET %B "root"
CALL &GROUP.FIND
OK? @found.group.1
  COPY %B %A
  ERROR "Failed to find group '%s'"
  JUMP @next.1
found.group.1:
CALL &GROUP.GET_GID
COPY %R %E
CALL &USERDB.CLOSE
COPY %F %A
COPY %D %B
COPY %E %C
CALL &FS.CHOWN
SET %D 0400
CALL &FS.CHMOD
next.1:
!FLAGGED? :changed @final.1
final.1:
EOF

gencode_ok "use host dir2.test", <<'EOF', "dir removal";
RESET
TOPIC "dir(/path/to/delete)"
SET %A "/path/to/delete"
CALL &FS.EXISTS?
NOTOK? @next.1
CALL &FS.IS_DIR?
OK? @isdir.1
  ERROR "%s exists, but is not a directory"
  JUMP @next.1
isdir.1:
CALL &FS.RMDIR
JUMP @next.1
next.1:
!FLAGGED? :changed @final.1
final.1:
EOF

gencode_ok "use host dir3.test", <<'EOF', "dir without chown";
RESET
TOPIC "dir(/chmod-me)"
SET %A "/chmod-me"
CALL &FS.EXISTS?
OK? @exists.1
  CALL &FS.MKDIR
  OK? @exists.1
  ERROR "Failed to create new directory '%s'"
  JUMP @next.1
exists.1:
CALL &FS.IS_DIR?
OK? @isdir.1
  ERROR "%s exists, but is not a directory"
  JUMP @next.1
isdir.1:
SET %D 0755
CALL &FS.CHMOD
next.1:
!FLAGGED? :changed @final.1
final.1:
EOF

gencode_ok "use host dir4.test", <<'EOF', "dir with non-root owner";
RESET
TOPIC "dir(/home/jrhunt/bin)"
SET %A "/home/jrhunt/bin"
CALL &FS.EXISTS?
OK? @exists.1
  CALL &FS.MKDIR
  OK? @exists.1
  ERROR "Failed to create new directory '%s'"
  JUMP @next.1
exists.1:
CALL &FS.IS_DIR?
OK? @isdir.1
  ERROR "%s exists, but is not a directory"
  JUMP @next.1
isdir.1:
COPY %A %F
SET %D 0
SET %E 0
CALL &USERDB.OPEN
OK? @owner.lookup.1
  ERROR "Failed to open the user database"
  HALT
owner.lookup.1:
SET %A 1
SET %B "jrhunt"
CALL &USER.FIND
OK? @found.user.1
  COPY %B %A
  ERROR "Failed to find user '%s'"
  JUMP @next.1
found.user.1:
CALL &USER.GET_UID
COPY %R %D
SET %A 1
SET %B "staff"
CALL &GROUP.FIND
OK? @found.group.1
  COPY %B %A
  ERROR "Failed to find group '%s'"
  JUMP @next.1
found.group.1:
CALL &GROUP.GET_GID
COPY %R %E
CALL &USERDB.CLOSE
COPY %F %A
COPY %D %B
COPY %E %C
CALL &FS.CHOWN
SET %D 0710
CALL &FS.CHMOD
next.1:
!FLAGGED? :changed @final.1
final.1:
EOF

done_testing;
