cmake_minimum_required(VERSION 3.20)

find_package(SQLite3 REQUIRED)

add_executable(lan-chat-server main.cpp)
target_link_libraries(lan-chat-server PRIVATE
    shared
    Boost::system
    SQLite::SQLite3
)
target_include_directories(lan-chat-server PRIVATE ${CMAKE_SOURCE_DIR})
