#!/bin/bash
SCRIPTPATH=$(cd $(dirname $0); pwd -P);
for PN in $(find t/tmp/data/pn/basics -name '*.pn'); do
	$SCRIPTPATH/verify ./pn --nofork $PN || exit $?
done
