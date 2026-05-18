cmake_minimum_required(VERSION 3.15)
project(PinscapePicoConfigTool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find libusb
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

include_directories(
    ${LIBUSB_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBProtocol
)

# Add library for USB abstraction and Vendor Interface
add_library(pinscape_usb STATIC
    USBDevice.cpp
    USBDevice.h
    VendorInterface.cpp
    VendorInterface.h
    FeedbackControllerInterface.cpp
    FeedbackControllerInterface.h
)

target_include_directories(pinscape_usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pinscape_usb PRIVATE
    ${LIBUSB_LIBRARIES}
)

# Add the main executable
add_executable(pinscape-config
    CmdLineConfigTool.cpp
    ../../Firmware/JSON.cpp
)

target_link_libraries(pinscape-config PRIVATE
    pinscape_usb
    ${LIBUSB_LIBRARIES}
)

# Installation
install(TARGETS pinscape-config
    RUNTIME DESTINATION bin
)

# Optional: Create a man page entry
if(UNIX AND NOT APPLE)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/Linux/pinscape-config.1
        DESTINATION share/man/man1
        OPTIONAL)
endif()

# Print build information
message(STATUS "Pinscape Pico Config Tool (Linux)")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "libusb found: ${LIBUSB_FOUND}")
message(STATUS "libusb version: ${LIBUSB_VERSION}")
