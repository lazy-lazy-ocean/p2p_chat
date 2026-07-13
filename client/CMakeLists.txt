cmake_minimum_required(VERSION 3.20)

find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.1)
pkg_check_modules(NOTIFY REQUIRED libnotify)

set(RESOURCES_DIR "${CMAKE_SOURCE_DIR}/client-resources")
set(COMBINED_HTML "${CMAKE_CURRENT_BINARY_DIR}/combined.html")
set(RESOURCES_HPP "${CMAKE_CURRENT_BINARY_DIR}/resources.hpp")

add_custom_command(
    OUTPUT ${COMBINED_HTML}
    COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/generate_resources.py"
        "${RESOURCES_DIR}/index.html"
        "${RESOURCES_DIR}/style.css"
        "${RESOURCES_DIR}/app.js"
        > ${COMBINED_HTML}
    DEPENDS
        "${RESOURCES_DIR}/index.html"
        "${RESOURCES_DIR}/style.css"
        "${RESOURCES_DIR}/app.js"
        "${CMAKE_SOURCE_DIR}/scripts/generate_resources.py"
    COMMENT "Combining HTML/CSS/JS..."
)

add_custom_command(
    OUTPUT ${RESOURCES_HPP}
    COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/generate_header.py"
        ${COMBINED_HTML} ${RESOURCES_HPP}
    DEPENDS ${COMBINED_HTML} "${CMAKE_SOURCE_DIR}/scripts/generate_header.py"
    COMMENT "Generating resource header..."
)

add_executable(lan-chat main.cpp ${RESOURCES_HPP})

target_include_directories(lan-chat PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}
    ${GTK3_INCLUDE_DIRS}
    ${WEBKIT_INCLUDE_DIRS}
    ${NOTIFY_INCLUDE_DIRS}
)

target_link_libraries(lan-chat PRIVATE
    shared
    Boost::system
    SQLite::SQLite3
    ${GTK3_LIBRARIES}
    ${WEBKIT_LIBRARIES}
    ${NOTIFY_LIBRARIES}
)

target_compile_options(lan-chat PRIVATE
    ${GTK3_CFLAGS_OTHER}
    ${WEBKIT_CFLAGS_OTHER}
    ${NOTIFY_CFLAGS_OTHER}
)

add_custom_target(gen_res DEPENDS ${RESOURCES_HPP})
add_dependencies(lan-chat gen_res)
