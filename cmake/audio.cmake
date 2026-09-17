# Build time generation of sound arrays from the files in sounds/.
#
# What the script produces is derived data, so it is built rather than
# committed: a quarter of a megabyte of hex per clip is not worth reviewing,
# and a checked in copy can disagree with the file it came from.
#
# ffmpeg has to be on PATH for anything that is not a WAV.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WAV2CPP "${PROJECT_ROOT}/tools/wav2cpp.py")

# Generated sources sit under one root that is on the include path, so they
# are reached as #include "pika/audio/sounds/meow.h".
set(GENERATED_SRC_ROOT "${CMAKE_BINARY_DIR}/generated_src")
set(GENERATED_SOUND_DIR "${GENERATED_SRC_ROOT}/pika/audio/sounds")

# pika_add_audio_sample(<target> SOUND <file> [NAME <id>] [RATE <hz>]
#                       [FORMAT hex|dec] [NAMESPACE <ns>])
#
# Adds the .cpp generated from SOUND to <target> and puts the generated tree
# on its include path. NAME defaults to the file's stem and is the C++
# identifier. Regenerates when the sound file or the script changes.
function(pika_add_sound target)
    cmake_parse_arguments(SND "" "SOUND;NAME;RATE;FORMAT;NAMESPACE" "" ${ARGN})

    if (NOT EXISTS "${SND_SOUND}")
        message(FATAL_ERROR "pika_add_audio_sample: no such sound: '${SND_SOUND}'")
    endif ()

    if (NOT SND_NAME)
        get_filename_component(SND_NAME "${SND_SOUND}" NAME_WE)
    endif ()

    if (NOT SND_RATE)
        set(SND_RATE 32000)
    endif ()
    if (NOT SND_FORMAT)
        set(SND_FORMAT hex)
    endif ()
    if (NOT SND_NAMESPACE)
        set(SND_NAMESPACE "pika::audio")
    endif ()

    set(gen_cpp "${GENERATED_SOUND_DIR}/${SND_NAME}.cpp")
    set(gen_h "${GENERATED_SOUND_DIR}/${SND_NAME}.h")

    add_custom_command(
            OUTPUT "${gen_cpp}" "${gen_h}"
            COMMAND ${Python3_EXECUTABLE} "${WAV2CPP}" "${SND_SOUND}"
            --rate ${SND_RATE}
            --name ${SND_NAME}
            --out-dir "${GENERATED_SOUND_DIR}"
            --namespace ${SND_NAMESPACE}
            --format ${SND_FORMAT}
            DEPENDS "${SND_SOUND}" "${WAV2CPP}"
            COMMENT "Generating ${SND_NAME} from ${SND_SOUND}"
            VERBATIM)

    # The header is a source too. Nothing compiles it, but it makes the target
    # depend on it, which is what stops a translation unit that includes it
    # from being compiled before it exists - on a clean tree there is no
    # header yet for CMake to find by scanning.
    target_sources(${target} PRIVATE "${gen_cpp}" "${gen_h}")
    target_include_directories(${target} PRIVATE "${GENERATED_SRC_ROOT}")
endfunction()
