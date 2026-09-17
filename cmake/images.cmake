# Build time generation of bitmap arrays from the files in images/.
#
# Like the sounds, what the script produces is derived data: a screenful of hex
# is not worth reviewing, and a checked in copy can disagree with the BMP it
# came from.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(BMP2CPP "${PROJECT_ROOT}/tools/bmp2cpp.py")

# Shares the root audio.cmake sets up, so a generated bitmap is reached as
# #include <pika/lcd/images/pika_logo.h>.
set(GENERATED_SRC_ROOT "${CMAKE_BINARY_DIR}/generated_src")
set(GENERATED_IMAGE_DIR "${GENERATED_SRC_ROOT}/pika/lcd/images")

# pika_add_image(<target> IMAGE <file> [NAME <id>] [NAMESPACE <ns>] [INVERT])
#
# Adds the .cpp generated from IMAGE to <target> and puts the generated tree on
# its include path. NAME defaults to the file's stem and is the C++ identifier.
# Regenerates when the BMP or the script changes.
function(pika_add_image target)
    cmake_parse_arguments(IMG "INVERT" "IMAGE;NAME;NAMESPACE" "" ${ARGN})

    if (NOT EXISTS "${IMG_IMAGE}")
        message(FATAL_ERROR "pika_add_image: no such image: '${IMG_IMAGE}'")
    endif ()

    if (NOT IMG_NAME)
        get_filename_component(IMG_NAME "${IMG_IMAGE}" NAME_WE)
    endif ()

    if (NOT IMG_NAMESPACE)
        set(IMG_NAMESPACE "pika::lcd")
    endif ()

    set(invert_arg "")
    if (IMG_INVERT)
        set(invert_arg --invert)
    endif ()

    set(gen_cpp "${GENERATED_IMAGE_DIR}/${IMG_NAME}.cpp")
    set(gen_h "${GENERATED_IMAGE_DIR}/${IMG_NAME}.h")

    add_custom_command(
            OUTPUT "${gen_cpp}" "${gen_h}"
            COMMAND ${Python3_EXECUTABLE} "${BMP2CPP}" "${IMG_IMAGE}"
            --name ${IMG_NAME}
            --out-dir "${GENERATED_IMAGE_DIR}"
            --namespace ${IMG_NAMESPACE}
            ${invert_arg}
            DEPENDS "${IMG_IMAGE}" "${BMP2CPP}"
            COMMENT "Generating ${IMG_NAME} from ${IMG_IMAGE}"
            VERBATIM)

    # The header is a source too, for the reason pika_add_sound gives: on a
    # clean tree there is no header yet for CMake's scan to find.
    target_sources(${target} PRIVATE "${gen_cpp}" "${gen_h}")
    target_include_directories(${target} PRIVATE "${GENERATED_SRC_ROOT}")
endfunction()
