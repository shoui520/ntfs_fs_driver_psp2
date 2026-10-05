#!/bin/sh
# Builds the host test, then runs it on a bare NTFS image and on an
# MBR-partitioned one, and checks the results with ntfsprogs.
# Needs: mkntfs, ntfscp, ntfscat, ntfsls, ntfsfix (ntfs-3g / ntfsprogs).
#
# Cortex-A9 (the Vita CPU) under qemu-user:
#   CC=arm-linux-gnueabihf-gcc SAN= EXTRA_CFLAGS="-mcpu=cortex-a9 -mthumb" \
#   RUN="qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf" ./run.sh
set -eu
cd "$(dirname "$0")"
O=${O:-build}
RUN=${RUN:-}
make -s O="$O"
# vnode/file lifetime belongs to iofilemgr, which the mocks do not model
export ASAN_OPTIONS=detect_leaks=0

mkimg() { # <file> <MiB> [sector size]
	rm -f "$1"
	truncate -s "${2}M" "$1"
	mkntfs -q -F -Q -s "${3:-512}" -L ntfsfs-test "$1" >/dev/null
	printf 'hello from ntfsprogs\n' > "$O/hello.txt"
	ntfscp -q "$1" "$O/hello.txt" hello.txt
}

check() { # <ntfs image>
	ntfsfix -n "$1" >/dev/null
	test "$(ntfscat "$1" from_vita.txt)" = "written by ntfsfs"
	ntfsls "$1" | grep -q '日本語フォルダ'
	! ntfsls "$1" | grep -qx dir
	echo "ntfsprogs check OK"
}

echo "== bare volume"
mkimg "$O/bare.img" 64
$RUN "$O/test_ntfsfs" "$O/bare.img"
check "$O/bare.img"

echo "== MBR partitioned disk (partition at 1 MiB)"
mkimg "$O/part.img" 64
python3 - "$O/disk.img" <<'PY'
import struct, sys
mbr = bytearray(512)
start, size = 2048, 64 * 2048
mbr[446:462] = bytes([0, 0, 0, 0, 0x07, 0, 0, 0]) + struct.pack('<II', start, size)
mbr[510:512] = b'\x55\xaa'
open(sys.argv[1], 'wb').write(bytes(mbr) + bytes(start * 512 - 512))
PY
cat "$O/part.img" >> "$O/disk.img"
$RUN "$O/test_ntfsfs" "$O/disk.img"
dd if="$O/disk.img" of="$O/part.img" bs=1M skip=1 status=none
check "$O/part.img"

echo "== 4096-byte sector device (some USB disks), bare and MBR"
mkimg "$O/bare4k.img" 64 4096
NTFSFS_TEST_SECTOR=4096 $RUN "$O/test_ntfsfs" "$O/bare4k.img"
check "$O/bare4k.img"
mkimg "$O/part4k.img" 64 4096
python3 - "$O/disk4k.img" <<'PY'
import struct, sys
mbr = bytearray(4096)
start, size = 256, 64 * 256          # LBAs in 4096-byte sectors
mbr[446:462] = bytes([0, 0, 0, 0, 0x07, 0, 0, 0]) + struct.pack('<II', start, size)
mbr[510:512] = b'\x55\xaa'
open(sys.argv[1], 'wb').write(bytes(mbr) + bytes(start * 4096 - 4096))
PY
cat "$O/part4k.img" >> "$O/disk4k.img"
NTFSFS_TEST_SECTOR=4096 $RUN "$O/test_ntfsfs" "$O/disk4k.img"
dd if="$O/disk4k.img" of="$O/part4k.img" bs=1M skip=1 status=none
check "$O/part4k.img"

echo "== read-only mount"
$RUN "$O/test_ntfsfs" "$O/bare.img" ro
echo "all tests passed"
