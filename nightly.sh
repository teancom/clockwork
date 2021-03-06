#!/bin/sh

if [ -f Makefile ]; then
	make distclean
fi

./autogen.sh
export CFLAGS=-g
./configure --prefix /cw
make
make DESTDIR=_nightly install
cp -R t/data/lxc _nightly/cw/cfm
cd _nightly;
tar -czf ../nightly.tar.gz *
cd ..
rm -rf _nightly

if [ "x$EXPORT" != "x" ]; then
	echo
	echo "Ready."
	echo
	echo "To load into a nightly LXC tester,"
	echo "run \`cd /; nc `hostname -f` $EXPORT | tar -xzv\`"
	echo "from inside the container environment."
	nc -l $EXPORT < nightly.tar.gz
fi
