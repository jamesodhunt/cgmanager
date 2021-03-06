#!/bin/bash
if [ -n "$SUDO_USER" ]; then
	gid=$SUDO_GID
	uid=$SUDO_UID
else
	gid=1000
	uid=1000
fi

echo "test 13: valid unprivileged setvalue"

mount -t cgroup -o memory cgroup /sys/fs/cgroup
prev=`cat /sys/fs/cgroup/zzz/b/memory.limit_in_bytes`
umount /sys/fs/cgroup

sudo -u \#$uid dbus-send --print-reply --address=unix:path=/tmp/cgmanager --type=method_call /org/linuxcontainers/cgmanager org.linuxcontainers.cgmanager0_0.setValue string:'memory' string:'zzz/b' string:'memory.limit_in_bytes' string:'99999' > /dev/null 2>&1
if [ $? -ne 0 ]; then
	exit 1
fi

mount -t cgroup -o memory cgroup /sys/fs/cgroup
after=`cat /sys/fs/cgroup/zzz/b/memory.limit_in_bytes`
umount /sys/fs/cgroup
if [ $prev = $after ]; then
	exit 1
fi

exit 0
