# Speaker, microphone, the shared sample clock and the MELPe recorder.
# MelpRecorder needs MELPE_SOURCES in the same target.

file(GLOB PIKA_AUDIO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
