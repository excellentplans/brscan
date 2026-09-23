# Copyright 2025 excellentplans. SPDX-License-Identifier: GPL-2.0-or-later
# run_replay_test.cmake — run a brscan5 replay e2e test and verify the
# output md5 and size against the reference fixture.
#
# Variables (passed via -D):
#   TEST_BIN     the test executable (test_brscan5_replay_raw or
#                test_brscan5_faults)
#   BACKEND_SO   the built libsane-brother.so (path relative to build dir)
#   OUT_FILE     where the test writes the page (decoded raw scanlines)
#   FIXTURE      the .tlv replay fixture
#   REF_MD5      expected md5 of the output (empty: skip md5/size check)
#   REF_SIZE     expected output size in bytes
#   SANE_CFG     backend config dir (tests/sane)
#   ARGS         optional extra args appended after <bin> <so> <out>
#                (test_brscan5_faults: mode as first argument)

if(NOT EXISTS "${FIXTURE}")
    message(FATAL_ERROR "replay fixture not found: ${FIXTURE} "
        "(missing fixture — see tests/data/brscan5/)")
endif()

# Resolve the backend .so to an absolute path in the build tree.
get_filename_component(BACKEND_ABS "${BACKEND_SO}" ABSOLUTE)
get_filename_component(BUILD_DIR "${BACKEND_ABS}" DIRECTORY)
get_filename_component(SANE_CFG_ABS "${SANE_CFG}" ABSOLUTE)
get_filename_component(FIXTURE_ABS "${FIXTURE}" ABSOLUTE)

file(REMOVE "${OUT_FILE}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
        "SANE_CONFIG_DIR=${SANE_CFG_ABS}"
        "LD_LIBRARY_PATH=${BUILD_DIR}"
        "BROTHER5_REPLAY=${FIXTURE_ABS}"
        # SANE_DEBUG_BROTHER=30 keeps the brscan5 DBG(1) failure
        # diagnostics visible in the ctest output on failure. The test
        # verdict itself never depends on log visibility — DBG output is
        # gated by the env var only.
        "SANE_DEBUG_BROTHER=30"
        "${TEST_BIN}" "${BACKEND_ABS}" "${OUT_FILE}" ${ARGS}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "test_brscan5_replay failed (rc=${rc})\n"
        "stdout:\n${stdout}\nstderr:\n${stderr}")
endif()

if(NOT EXISTS "${OUT_FILE}")
    message(FATAL_ERROR "test produced no output file: ${OUT_FILE}\n${stdout}")
endif()

if(REF_MD5 STREQUAL "")
    message(STATUS "brscan5 replay test OK (no md5 check): ${OUT_FILE}")
    return()
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E md5sum "${OUT_FILE}"
    OUTPUT_VARIABLE md5line
    OUTPUT_STRIP_TRAILING_WHITESPACE)
# md5sum output is "<md5>  <file>"
string(REGEX MATCH "^[0-9a-fA-F]+" OUT_MD5 "${md5line}")

if(NOT OUT_MD5 STREQUAL REF_MD5)
    message(FATAL_ERROR "md5 mismatch: got ${OUT_MD5}, want ${REF_MD5}\n"
        "${stdout}")
endif()

file(SIZE "${OUT_FILE}" OUT_SIZE)
if(NOT OUT_SIZE EQUAL REF_SIZE)
    message(FATAL_ERROR "size mismatch: got ${OUT_SIZE}, want ${REF_SIZE}\n"
        "${stdout}")
endif()

message(STATUS "brscan5_replay_raw e2e OK: ${OUT_MD5} (${OUT_SIZE} B, ${OUT_FILE})")