#!/usr/bin/perl
use strict;
use warnings;

use Test::More;
use t::common;

gencode_ok "use host deps1.test", <<'EOF', "implict deps";
RESET
TOPIC "dir(/tmp)"
SET %A "/tmp"
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
next.1:
!FLAGGED? :changed @final.1
  FLAG 1 :res2
  FLAG 1 :res3
final.1:
RESET
TOPIC "dir(/tmp/inner)"
SET %A "/tmp/inner"
CALL &FS.EXISTS?
OK? @exists.2
  CALL &FS.MKDIR
  OK? @exists.2
  ERROR "Failed to create new directory '%s'"
  JUMP @next.2
exists.2:
CALL &FS.IS_DIR?
OK? @isdir.2
  ERROR "%s exists, but is not a directory"
  JUMP @next.2
isdir.2:
next.2:
!FLAGGED? :changed @final.2
  FLAG 1 :res2
final.2:
RESET
TOPIC "file(/tmp/inner/file)"
SET %A "/tmp/inner/file"
CALL &FS.EXISTS?
OK? @exists.3
  CALL &FS.MKFILE
  OK? @exists.3
  ERROR "Failed to create new file '%s'"
  JUMP @next.3
exists.3:
CALL &FS.IS_FILE?
OK? @isfile.3
  ERROR "%s exists, but is not a regular file"
  JUMP @next.3
isfile.3:
next.3:
!FLAGGED? :changed @final.3
final.3:
EOF



done_testing;
