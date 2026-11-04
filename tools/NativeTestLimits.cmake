# Per-test run limits from measured clean durations (tools/ci-test-durations.json).
# A test without a measurement keeps the historical 600-second limit.
if(NOT DEFINED RACKSYNTH_TEST_DURATIONS_FILE)
    set(RACKSYNTH_TEST_DURATIONS_FILE "${CMAKE_CURRENT_LIST_DIR}/ci-test-durations.json")
endif()
set(RACKSYNTH_DEFAULT_RUN_SECONDS 600)
# CTest slots reserved by each of the heavy GUI/CPU cases. Eight of sixteen admits two at once.
# Four was tried on 2026-10-30 (run 41035170952) and coincided with desktop timeouts; keep eight
# until a quiet-machine measurement shows a lower value holds.
set(RACKSYNTH_HEAVY_TEST_PROCESSORS 8 CACHE STRING "CTest PROCESSORS reserved by each heavy native test")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    # Editing the table re-registers the tests on the next build.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RACKSYNTH_TEST_DURATIONS_FILE}")
endif()

function(racksynth_test_run_seconds name out)
    set(limit ${RACKSYNTH_DEFAULT_RUN_SECONDS})
    if(EXISTS "${RACKSYNTH_TEST_DURATIONS_FILE}")
        file(READ "${RACKSYNTH_TEST_DURATIONS_FILE}" table)
        string(JSON headroom ERROR_VARIABLE missing GET "${table}" headroom)
        string(JSON floor ERROR_VARIABLE missing_floor GET "${table}" floorSeconds)
        string(JSON seconds ERROR_VARIABLE unmeasured GET "${table}" tests "${name}")
        if(NOT missing AND NOT missing_floor AND NOT unmeasured)
            # Integer math: ceil(seconds * headroom), clamped to [floor, default].
            math(EXPR scaled "(${seconds} * ${headroom} + 99) / 100")
            if(scaled LESS floor)
                set(scaled ${floor})
            endif()
            if(scaled LESS limit)
                set(limit ${scaled})
            endif()
        endif()
    endif()
    set(${out} ${limit} PARENT_SCOPE)
endfunction()
