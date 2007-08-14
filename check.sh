#!/bin/sh

function test_result() {
    if test "$?" != "0" ; then
	echo Test $i failed
	exit 1
    else
	echo Test $i successful
    fi
    i=$((i + 1))
}


i=1

# Test start range
echo Test $i
out=$(echo foo |./pmr -i 1: 2>/dev/null)
test "$out" == "oo"
test_result $i

echo Test $i
out=$(echo foo |./pmr -i 3: 2>/dev/null)
test -z "$out"
test_result $i

# Test end range
echo Test $i
out=$(echo foo |./pmr -i :1 2>/dev/null)
test "$out" = "f"
test_result $i

echo Test $i
out=$(echo foo |./pmr -i :0 2>/dev/null)
test -z "$out"
test_result $i

# Test both start and end range
echo Test $i
out=$(echo foo |./pmr -i 0:0 2>/dev/null)
test -z "$out"
test_result $i

echo Test $i
out=$(echo foo |./pmr -i 1:1 2>/dev/null)
test -z "$out"
test_result $i

echo Test $i
out=$(echo foo |./pmr -i 1:3 2>/dev/null)
test "$out" = "oo"
test_result $i

echo Test $i
dd if=/dev/zero bs=1000000 count=3 2>/dev/null |./pmr -l 1000000 > /dev/zero 2>/dev/null
test_result $i
