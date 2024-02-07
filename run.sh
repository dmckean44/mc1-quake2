#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
pushd "${SCRIPT_DIR}" > /dev/null

EXE_FILE=out/release/quake2
PROFILE_FILE=""
SIM_ARGS=""

# Collect script arguments.
while true ; do
    if [ "$1" == "--profile" ] ; then
        shift 1
        PROFILE_FILE=/tmp/mr32sim-sym-$$
        mrisc32-elf-readelf -sW "${EXE_FILE}" | grep FUNC | awk '{print $2,$8}' > "${PROFILE_FILE}"
        SIM_ARGS="${SIM_ARGS} -P ${PROFILE_FILE}"
    elif [ "$1" == "-f" ] ; then
        shift 1
        SIM_ARGS="${SIM_ARGS} -f"
    elif [ "$1" == "-v" ] ; then
        shift 1
        SIM_ARGS="${SIM_ARGS} -v"
    else
        break
    fi
done

# Run the executable in the simulator.
mr32sim -v -g -gw 320 -gh 180 -gd 8 -ga 0x400029d4 -gp 0x4000202c ${SIM_ARGS} "${EXE_FILE}" +set r_mode 0 +set cl_showfps 1 "$@"

# Delete the temporary profiling data.
if [ -n "${PROFILE_FILE}" ] ; then
    rm "${PROFILE_FILE}"
fi

popd > /dev/null

