#!/bin/bash

echo "Test 2 (getMyCgroup)"
dbus-send --print-reply --address=unix:path=/tmp/cgmanager --type=method_call /org/linuxcontainers/cgmanager org.linuxcontainers.cgmanager0_0.getMyCgroup string:'memory' > /dev/null 2>&1
if [ $? -ne 0 ]; then
	exit 1
fi
exit 0
