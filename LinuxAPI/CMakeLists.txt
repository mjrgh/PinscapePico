cmake_minimum_required(VERSION 3.15)
project(PinscapeLinuxAPI)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find libusb
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

include_directories(
    ${LIBUSB_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../USBProtocol
)

# Add library for USB abstraction, Vendor Interface, and Feedback Controller
add_library(pinscape_linux_api STATIC
    USBDevice.cpp
    USBDevice.h
    PinscapeVendorInterface.cpp
    PinscapeVendorInterface.h
    FeedbackControllerInterface.cpp
    FeedbackControllerInterface.h
)

target_include_directories(pinscape_linux_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pinscape_linux_api PRIVATE
    ${LIBUSB_LIBRARIES}
)

# Print build information
message(STATUS "Pinscape Pico Linux API Library")
message(STATUS "libusb found: ${LIBUSB_FOUND}")
message(STATUS "libusb version: ${LIBUSB_VERSION}")
