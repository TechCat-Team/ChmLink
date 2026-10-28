cmake_minimum_required(VERSION 3.16)
project(chmlink LANGUAGES C)

add_library(chmlink_packet STATIC src/packet/packet.c)
target_include_directories(chmlink_packet PUBLIC include)
target_compile_features(chmlink_packet PUBLIC c_std_99)

include(CTest)
if(BUILD_TESTING)
    add_executable(packet_test tests/packet_test.c)
    target_link_libraries(packet_test PRIVATE chmlink_packet)
    add_test(NAME packet_test COMMAND packet_test)
endif()
