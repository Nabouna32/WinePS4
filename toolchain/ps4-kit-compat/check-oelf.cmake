# Check the OELF created by OpenOrbis create-fself before it is packaged.
if (NOT DEFINED OELF OR NOT EXISTS "${OELF}")
    message(FATAL_ERROR "check-oelf: OELF does not exist: ${OELF}")
endif ()
find_program(PS4_READELF NAMES llvm-readelf llvm-readelf-21 llvm-readelf-12 readelf
    HINTS "${OPENORBIS}/bin")
if (NOT PS4_READELF)
    message(FATAL_ERROR "check-oelf: no readelf found; refusing to package an unchecked OELF")
endif ()
execute_process(COMMAND "${PS4_READELF}" -l -W "${OELF}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if (NOT _rc EQUAL 0)
    message(FATAL_ERROR "check-oelf: readelf failed: ${_err}")
endif ()
string(REGEX MATCHALL "\\n *LOAD +[^\\n]* RW  [^\\n]*" _rw_loads "${_out}")
list(LENGTH _rw_loads _rw_count)
if (NOT _rw_count EQUAL 1)
    message(FATAL_ERROR
        "check-oelf: expected exactly one read-write PT_LOAD, found ${_rw_count}. "
        "This OELF may be rejected by the console.\n${_out}")
endif ()
message(STATUS "check-oelf: ${OELF}: one read-write PT_LOAD")
