#!/bin/bash
# Classify every TRFTuner vtable slot by its first few instructions.
#
# Most of a class this size is accessors, and an accessor gives itself away
# immediately: "ldr r0,[r0,#N]; bx lr" is a getter for the field at N, and the
# same shape with str is a setter. Those pin down the object layout for free.
# Anything that pushes registers and branches is real work and gets looked at
# properly. Doing all 38 in one pass is much cheaper than guessing which to
# open.
cd "$(dirname "$0")"

# slot:address, TRFTuner unless marked. The RTXC column where it overrides,
# since that is the built-in tuner rather than the accessory one.
SLOTS="
2:0x08472ef1 3:0x084730c9 6:0x0841b03d 7:0x084527a1 8:0x0841a629
9:0x084527f7 10:0x084133b7 13:0x0847310d 14:0x08472fd9 15:0x0847497b
16:0x084749c1 17:0x08474981 19:0x08027885 20:0x08472f3d 21:0x084735c9
22:0x084730f1 23:0x08472fb9 24:0x08472e3f 25:0x08472dd9 26:0x084736f5
28:0x08474a2d 33:0x08474a17 34:0x08473069 35:0x084736b9 37:0x08472df9
"
RTXC="
0:0x08456991 1:0x08456983 18:0x08456637 27:0x08456929 29:0x08456633
30:0x0845663b 31:0x084564b5 32:0x084568d1 36:0x08456451
"

dump() {
	local slot=$1 addr=$2 tag=$3
	# Thumb bit is set in every one of these; clear it for the file offset.
	local a=$(( addr & ~1 ))
	printf '%-22s slot %-3s 0x%08x\n' "$tag" "$slot" "$a"
	./dis.sh "$a" 24 thumb 2>/dev/null | tail -n +2 | \
		sed 's/^ *[0-9a-f]*:\t[0-9a-f ]*\t/      /' | head -6
	echo
}

echo "################ TRFTuner (shared implementation) ################"
for e in $SLOTS; do dump "${e%%:*}" "${e##*:}" "TRFTuner"; done

echo "################ TRFTuner_RTXC (overrides) ################"
for e in $RTXC; do dump "${e%%:*}" "${e##*:}" "TRFTuner_RTXC"; done
