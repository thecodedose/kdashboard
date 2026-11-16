cmake_minimum_required(VERSION 3.13)
project(soloist_pulse_shim C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -O2)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

add_library(pulse SHARED
    src/mainloop.c
    src/support.c
    src/context.c
    src/stream.c
    src/ring.c
)

target_include_directories(pulse PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${ALSA_INCLUDE_DIRS}
)

target_link_libraries(pulse PRIVATE ${ALSA_LIBRARIES} m pthread)

set_target_properties(pulse PROPERTIES
    OUTPUT_NAME pulse
    SOVERSION 0
    VERSION 0.2.13
    LINK_FLAGS "-Wl,--version-script=${CMAKE_SOURCE_DIR}/pulse.sym"
)

install(TARGETS pulse LIBRARY DESTINATION lib)

option(BUILD_TESTING "Build the shim regression tests" OFF)
include(CTest)
if(BUILD_TESTING)
    add_executable(shim_timing_test
        test/timing.c
        src/mainloop.c
        src/support.c
        src/context.c
        src/ring.c
    )
    target_include_directories(shim_timing_test PRIVATE include src ${ALSA_INCLUDE_DIRS})
    target_link_libraries(shim_timing_test PRIVATE ${ALSA_LIBRARIES} m pthread)
    set_target_properties(shim_timing_test PROPERTIES LINK_FLAGS
        "-Wl,--wrap=snd_pcm_delay,--wrap=snd_pcm_avail,--wrap=snd_pcm_state,--wrap=snd_pcm_prepare,--wrap=snd_pcm_writei,--wrap=snd_pcm_nonblock,--wrap=snd_pcm_drop,--wrap=snd_pcm_close")
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_FLAGS MATCHES "-fsanitize=[^ ]*address")
        target_compile_options(shim_timing_test PRIVATE -fno-pie)
        set_property(TARGET shim_timing_test APPEND_STRING PROPERTY LINK_FLAGS " -no-pie")
    endif()
    add_test(NAME timing-completion COMMAND shim_timing_test completion)
    add_test(NAME timing-lifecycle COMMAND shim_timing_test lifecycle)
    add_test(NAME timing-precision COMMAND shim_timing_test precision)
    add_test(NAME timing-drain COMMAND shim_timing_test drain)
    add_test(NAME timing-dryspell COMMAND shim_timing_test dryspell)
    set_tests_properties(timing-completion timing-lifecycle timing-precision timing-drain timing-dryspell PROPERTIES TIMEOUT 30)
endif()
