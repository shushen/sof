#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause

usage ()
{
    echo "Usage:   $0 <bits in> <bits out> <rate> <input> <output>"
    echo "Example: $0 16 16 48000 input.raw output.raw"
}

main ()
{
    local COMP DIRECTION

    if [ $# -ne 5 ]; then
	usage "$0"
	exit
    fi

    COMP=dcblock
    DIRECTION=playback

    ./comp_run.sh $COMP $DIRECTION "$1" "$2" "$3" "$3" "$4" "$5"
}

main "$@"
