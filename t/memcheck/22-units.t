#!/bin/bash
SCRIPTPATH=$(cd $(dirname $0); pwd -P);
for TEST in $(find t -maxdepth 1 -type f -perm /u=x | sort); do
	$SCRIPTPATH/verify $TEST || exit $?
done
