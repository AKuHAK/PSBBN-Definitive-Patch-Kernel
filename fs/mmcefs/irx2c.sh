#!/bin/sh
# Print IRX files as C arrays for mmcehd.c: irx/iomanX.irx -> irx_iomanx[]
for f in "$@"; do
	n=`basename "$f" .irx | tr A-Z a-z`
	echo "static const unsigned char irx_$n[] = {"
	od -An -v -tx1 "$f" | sed -e 's/ *\([0-9a-f][0-9a-f]\)/0x\1,/g'
	echo "};"
done
