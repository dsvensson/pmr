#!/bin/sh

# Test skipping
out=$(echo foo |./pmr -i 1: 2>/dev/null |wc |grep '[ ]*1[ ]*1[ ]*3')
if test -z "$out" ; then
    echo Test 1 failed
    exit 1
fi
echo Test 1 successful

dd if=/dev/zero bs=1000000 count=7 |./pmr -l 1000000 > /dev/zero
echo Test 2 successful
