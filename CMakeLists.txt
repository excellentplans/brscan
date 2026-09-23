cmake_minimum_required(VERSION 3.5)

project(brscan)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Sane REQUIRED)
find_package(LibUSB REQUIRED)
find_package(JPEG REQUIRED)

add_library(brscandec SHARED
	libbrscandec/brother_bugchk.c
	libbrscandec/brother_scandec.c
	libbrscandec/brother_chreso.c)
set_target_properties(brscandec PROPERTIES SOVERSION 1)
# Ghidra decompilation uses undefined8/undefined4 generics that mismatch
# pointer vs integer types. These are semantically correct on LP64
# (pointers and uint64_t are both 8 bytes). GCC 14+ treats these as errors.
target_compile_options(brscandec PRIVATE
	-Wno-incompatible-pointer-types
	-Wno-int-conversion
	-Wno-pointer-sign)

add_library(brcolm SHARED
	libbrcolm/brcolm.c)
set_target_properties(brcolm PROPERTIES SOVERSION 1)

add_library(sane-brother SHARED
	libsane-brother/brother.c
	libsane-brother/brother_advini.c
	libsane-brother/brscan5_proto.c
	libsane-brother/brother_brscan4.c
	libsane-brother/brother_brscan5.c
	libsane-brother/brother_brscan5_replay.c
	libsane-brother/brother_bugchk.c
	libsane-brother/brother_cmatch.c
	libsane-brother/brother_color.c
	libsane-brother/brother_devaccs.c
	libsane-brother/brother_devinfo.c
	libsane-brother/brother_log.c
	libsane-brother/brother_mfccmd.c
	libsane-brother/brother_misc.c
	libsane-brother/brother_modelinf.c
	libsane-brother/brother_netdev.c
	libsane-brother/brother_scanner.c
	libsane-brother/brscan5_stream.c
	libsane-brother/sane_strstatus.c
	libsane-brother/sanei_config.c
	libsane-brother/sanei_constrain_value.c
	libsane-brother/sanei_init_debug.c)
# stubs.c provides sane_init→sane_brother_init wrappers.
# Compiled as a plain C file without BACKEND_NAME to avoid macro conflicts.
add_library(sane-brother-stubs OBJECT libsane-brother/stubs.c)
target_include_directories(sane-brother-stubs PRIVATE ${SANE_INCLUDE_DIR})
set_target_properties(sane-brother-stubs PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(sane-brother PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/libbrscandec
	${CMAKE_CURRENT_SOURCE_DIR}/libbrcolm
	${SANE_INCLUDE_DIR}
	${LIBUSB_INCLUDE_DIR}
	${JPEG_INCLUDE_DIRS})
target_compile_definitions(sane-brother PRIVATE BACKEND_NAME=brother BRSANESUFFIX=2 V_MAJOR=1 V_MINOR=0)
target_link_libraries(sane-brother dl pthread usb ${LIBUSB_LIBRARIES} ${JPEG_LIBRARIES} sane-brother-stubs)
set_target_properties(sane-brother PROPERTIES SOVERSION 1)

# BRSANESUFFIX=2 code loads libbrscandec2.so and libbrcolm2.so by name
install(CODE "
  execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink libbrscandec.so \${CMAKE_INSTALL_PREFIX}/lib/libbrscandec2.so)
  execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink libbrcolm.so \${CMAKE_INSTALL_PREFIX}/lib/libbrcolm2.so)
")

add_executable(brsaneconfig
        brsaneconfig/brsaneconfig.c)
target_include_directories(brsaneconfig PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libsane-brother)
target_compile_definitions(brsaneconfig PRIVATE BRSANESUFFIX=2)
target_link_libraries(brsaneconfig sane-brother)

# brscan5 command-layer unit tests (plain C, no framework, independent of
# the backend build — links the pure protocol module directly, no backend
# deps — and verifies the encoder output byte-exactly against
# commands.golden.json).
add_executable(test_brscan5_encoders
        tests/unit/test_brscan5_encoders.c
        libsane-brother/brscan5_proto.c)
target_include_directories(test_brscan5_encoders PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libsane-brother
        ${CMAKE_CURRENT_SOURCE_DIR}/libbrscandec
        ${CMAKE_CURRENT_SOURCE_DIR}/libbrcolm
        ${SANE_INCLUDE_DIR})
target_compile_definitions(test_brscan5_encoders PRIVATE BRSANESUFFIX=2)
enable_testing()
add_test(NAME brscan5_encoders COMMAND test_brscan5_encoders)

# Scan-geometry conversion policies: estimate path (0.1-mm ints,
# xdpi/254 truncation + clamp) and XSC AREA path (raw SANE mm,
# xdpi/25.4 round-half-up), incl. the documented T8c 600-dpi AREA
# divergence (8400 computed vs 8399 captured — see the test header).
add_executable(test_brscan5_geo
        tests/unit/test_brscan5_geo.c
        libsane-brother/brscan5_proto.c)
target_include_directories(test_brscan5_geo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libsane-brother
        ${CMAKE_CURRENT_SOURCE_DIR}/libbrscandec
        ${CMAKE_CURRENT_SOURCE_DIR}/libbrcolm
        ${SANE_INCLUDE_DIR})
target_compile_definitions(test_brscan5_geo PRIVATE BRSANESUFFIX=2)
add_test(NAME brscan5_geo COMMAND test_brscan5_geo)

add_executable(test_brscan5_stream_rle
        tests/unit/test_brscan5_stream_rle.c
        libsane-brother/brscan5_stream.c)
target_include_directories(test_brscan5_stream_rle PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libsane-brother)
add_test(NAME brscan5_stream_rle COMMAND test_brscan5_stream_rle)

# T8e: status records riding INSIDE data chunks (USB short-packet rule) —
# the collected JPEG must stay byte-identical in every coalescing pattern.
add_executable(test_brscan5_stream_records
        tests/unit/test_brscan5_stream_records.c
        libsane-brother/brscan5_stream.c)
target_include_directories(test_brscan5_stream_records PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libsane-brother)
add_test(NAME brscan5_stream_records COMMAND test_brscan5_stream_records)

# Synthetic edge-case tests for the stream parser (ported from the
# standalone parser suite): chunking at arbitrary split points, EOI split
# across chunks, JPEG buffer overflow, idle feed, multipage, trailing
# data, unknown records, and a randomized fuzz loop. The fuzz iteration
# count defaults to 5000; CI can raise it with
# -DBRSCAN5_FUZZ_ITERATIONS=<N> (or per run: argv[1]).
set(BRSCAN5_FUZZ_ITERATIONS 5000 CACHE STRING
        "brscan5 stream parser fuzz iterations (default 5000)")
add_executable(test_brscan5_stream_edge
        tests/unit/test_brscan5_stream_edge.c
        libsane-brother/brscan5_stream.c)
target_include_directories(test_brscan5_stream_edge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libsane-brother)
target_compile_definitions(test_brscan5_stream_edge PRIVATE
        FUZZ_ITERATIONS=${BRSCAN5_FUZZ_ITERATIONS})
add_test(NAME brscan5_stream_edge COMMAND test_brscan5_stream_edge)

# brscan5 replay end-to-end test (T6): loads the built backend .so, runs the
# full SANE lifecycle against the replay transport (no hardware), sets
# resolution/mode via sane_control_option, checks the two-phase
# sane_get_parameters behaviour, and writes the DECODED raw scanlines
# (libjpeg streaming read path) to a temp file. The output is compared
# against an independent libjpeg decoding of the reference JPEG fixture
# (2446x3485 RGB, md5 below; generated with a standalone libjpeg dumper +
# cross-checked with PIL). The T5 JPEG-passthrough test was removed: since
# the streaming read path there is no JPEG passthrough anymore.
# TLV replay fixtures: tests/data/brscan5/ (format documented in docs/brscan5-protocol.md)
set(BR5_REPLAY_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/brscan5/fixture-color-300.tlv")
set(BR5_RAW_REF_MD5 "f1509e79819fef7a5d44e9be5812a1b1")
set(BR5_RAW_REF_SIZE "25572930")
add_executable(test_brscan5_replay_raw tests/test_brscan5_replay_raw.c)
target_include_directories(test_brscan5_replay_raw PRIVATE ${SANE_INCLUDE_DIR})
target_link_libraries(test_brscan5_replay_raw dl)
add_test(NAME brscan5_replay_raw
	COMMAND ${CMAKE_COMMAND}
		-D TEST_BIN=$<TARGET_FILE:test_brscan5_replay_raw>
		-D BACKEND_SO=$<TARGET_FILE:sane-brother>
		-D OUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/replay-test.raw
		-D FIXTURE=${BR5_REPLAY_FIXTURE}
		-D REF_MD5=${BR5_RAW_REF_MD5}
		-D REF_SIZE=${BR5_RAW_REF_SIZE}
		-D SANE_CFG=${CMAKE_CURRENT_SOURCE_DIR}/tests/sane
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_replay_test.cmake)

# T8e: same colour-300 cycle, but the mid-stream 00 02 progress record
# rides at the TAIL of a 262144-B data URB (hardware USB short-packet
# race). The parser must strip it — the decoded output is byte-identical
# to the clean fixture (same md5/size as brscan5_replay_raw).
set(BR5_RECMERGE_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/brscan5/fixture-color-300-recmerge.tlv")
add_test(NAME brscan5_replay_recmerge
	COMMAND ${CMAKE_COMMAND}
		-D TEST_BIN=$<TARGET_FILE:test_brscan5_replay_raw>
		-D BACKEND_SO=$<TARGET_FILE:sane-brother>
		-D OUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/replay-test-recmerge.raw
		-D FIXTURE=${BR5_RECMERGE_FIXTURE}
		-D REF_MD5=${BR5_RAW_REF_MD5}
		-D REF_SIZE=${BR5_RAW_REF_SIZE}
		-D SANE_CFG=${CMAKE_CURRENT_SOURCE_DIR}/tests/sane
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_replay_test.cmake)
set_tests_properties(brscan5_replay_recmerge PROPERTIES TIMEOUT 120)

# T8d BW/RLENGTH replay end-to-end test: fixture-bw300.tlv (chroot BW-300
# reference capture, packbits blocks) -> decoded 1-bit bitmap. The expected
# md5 is pinned after the first green run; the fixture is regenerated by
# make_replay_fixture.py --bw300 (byte-identical to the T8c original).
set(BR5_BW_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/brscan5/fixture-bw300.tlv")
set(BR5_BW_REF_MD5 "a33cee74f20e2e60bffc57adfe54b176")
set(BR5_BW_REF_SIZE "1113629")
add_executable(test_brscan5_replay_bw tests/test_brscan5_replay_bw.c)
target_include_directories(test_brscan5_replay_bw PRIVATE ${SANE_INCLUDE_DIR})
target_link_libraries(test_brscan5_replay_bw dl)
add_test(NAME brscan5_replay_bw
	COMMAND ${CMAKE_COMMAND}
		-D TEST_BIN=$<TARGET_FILE:test_brscan5_replay_bw>
		-D BACKEND_SO=$<TARGET_FILE:sane-brother>
		-D OUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/replay-test-bw.raw
		-D FIXTURE=${BR5_BW_FIXTURE}
		-D REF_MD5=${BR5_BW_REF_MD5}
		-D REF_SIZE=${BR5_BW_REF_SIZE}
		-D SANE_CFG=${CMAKE_CURRENT_SOURCE_DIR}/tests/sane
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_replay_test.cmake)
set_tests_properties(brscan5_replay_bw PROPERTIES TIMEOUT 60)

# T7 fault-injection e2e tests (cancel mid-page, transport error, simulated
# timeout, empty feeder). TLV replay fixtures: tests/data/brscan5/
# (format documented in docs/brscan5-protocol.md):
#   --cancel-midpage -> fixture-cancel-midpage.tlv  (2 full cycles; cancel
#                       after ~10 % of the read loop, restart w/o reopen)
#   --error-midpage  -> fixture-error-midpage.tlv   (0x04 error ~10 % into
#                       the data phase, then a clean 2nd cycle)
#   --timeout        -> fixture-timeout.tlv         (0x03 timeout sim)
#   --nodocs         -> fixture-nodocs.tlv          (CKD 00 02 -> NO_DOCS)
#   --jam            -> fixture-jam.tlv             (XSC 91 00 -> JAMMED)
# cancel/error write the SECOND cycle's page and are md5-checked against
# the reference (same page as brscan5_replay_raw).
set(BR5_FAULT_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/brscan5")
set(BR5_EMPTY_MD5 "d41d8cd98f00b204e9800998ecf8427e")
add_executable(test_brscan5_faults tests/test_brscan5_faults.c)
target_include_directories(test_brscan5_faults PRIVATE ${SANE_INCLUDE_DIR})
target_link_libraries(test_brscan5_faults dl)

foreach(fault IN ITEMS cancel error timeout nodocs jam)
  if(fault STREQUAL "timeout" OR fault STREQUAL "nodocs"
     OR fault STREQUAL "jam")
    set(br5_ref_md5 ${BR5_EMPTY_MD5})
    set(br5_ref_size 0)
    set(br5_fixture fixture-${fault}.tlv)
  else()
    set(br5_ref_md5 ${BR5_RAW_REF_MD5})
    set(br5_ref_size ${BR5_RAW_REF_SIZE})
    set(br5_fixture fixture-${fault}-midpage.tlv)
  endif()
  add_test(NAME brscan5_${fault}
	COMMAND ${CMAKE_COMMAND}
		-D TEST_BIN=$<TARGET_FILE:test_brscan5_faults>
		-D BACKEND_SO=$<TARGET_FILE:sane-brother>
		-D OUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/fault-${fault}.raw
		-D FIXTURE=${BR5_FAULT_FIXTURE_DIR}/${br5_fixture}
		-D ARGS=${fault}
		-D REF_MD5=${br5_ref_md5}
		-D REF_SIZE=${br5_ref_size}
		-D SANE_CFG=${CMAKE_CURRENT_SOURCE_DIR}/tests/sane
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_replay_test.cmake)
  # no hangs: every fault test must finish well within this budget
  set_tests_properties(brscan5_${fault} PROPERTIES TIMEOUT 60)
endforeach()

# AddressSanitizer for the brscan5 tests (T6: libjpeg buffer handling).
# Build in a separate directory: cmake -S . -B build-asan -DBRSCAN_ASAN=ON
option(BRSCAN_ASAN "Build libsane-brother + brscan5 tests with AddressSanitizer" OFF)
if(BRSCAN_ASAN)
  target_compile_options(sane-brother PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(sane-brother PRIVATE -fsanitize=address)
  target_compile_options(test_brscan5_replay_raw PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(test_brscan5_replay_raw PRIVATE -fsanitize=address)
  target_compile_options(test_brscan5_replay_bw PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(test_brscan5_replay_bw PRIVATE -fsanitize=address)
  target_compile_options(test_brscan5_faults PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(test_brscan5_faults PRIVATE -fsanitize=address)
  target_compile_options(test_brscan5_encoders PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(test_brscan5_encoders PRIVATE -fsanitize=address)
endif()

install(TARGETS brsaneconfig brscandec brcolm)
install(TARGETS sane-brother LIBRARY DESTINATION /usr/lib/sane)

install(FILES libbrcolm/GrayCmData/BHminiFB/brmfgray.bin DESTINATION /usr/share/sane/brother/GrayCmData/BHminiFB)
install(FILES libbrcolm/GrayCmData/YL4/brmfgray.bin DESTINATION /usr/share/sane/brother/GrayCmData/YL4)
install(FILES libbrcolm/GrayCmData/BHL2FB/brmsi06f.bin DESTINATION /usr/share/sane/brother/GrayCmData/BHL2FB)
install(FILES libbrcolm/GrayCmData/ZLe/brmfgray.bin DESTINATION /usr/share/sane/brother/GrayCmData/ZLe)
install(FILES libbrcolm/GrayCmData/BHL2/brmsi06.bin DESTINATION /usr/share/sane/brother/GrayCmData/BHL2)
install(FILES libbrcolm/GrayCmData/ZL2FB/brmsl07f.bin libbrcolm/GrayCmData/ZL2FB/brmsl07f.cm DESTINATION /usr/share/sane/brother/GrayCmData/ZL2FB)
install(FILES libbrcolm/GrayCmData/ZL2/brmsl07.bin DESTINATION /usr/share/sane/brother/GrayCmData/ZL2)
install(FILES libbrcolm/GrayCmData/BHLFB/brmfgray.bin DESTINATION /usr/share/sane/brother/GrayCmData/BHLFB)
install(FILES libbrcolm/GrayCmData/YL4FB/brlutcm.dat libbrcolm/GrayCmData/YL4FB/brmfgray.bin DESTINATION /usr/share/sane/brother/GrayCmData/YL4FB)
install(FILES libbrcolm/GrayCmData/BHL/brmfgray.bin DESTINATION /usr/share/sane/brother/GrayCmData/BHL)
install(FILES libbrcolm/GrayCmData/BHMFB/brmfgray.bin DESTINATION /usr/share/sane/brother/GrayCmData/BHMFB)
install(FILES libbrcolm/GrayCmData/ZLeFB/brlutcm.dat libbrcolm/GrayCmData/ZLeFB/brmfgray.bin DESTINATION /usr/share/sane/brother/GrayCmData/ZLeFB)

install(FILES data/brsanenetdevice.cfg data/Brsane.ini DESTINATION /usr/share/sane/brother)
# BRSANESUFFIX=2 looks for Brsane2.ini
install(CODE "
  execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink Brsane.ini \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/share/sane/brother/Brsane2.ini)
")

