cmake_minimum_required(VERSION 3.20)
project(AutoKeyDown LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the .exe runs on machines without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(autokeydown WIN32 src/main.cpp)
target_compile_definitions(autokeydown PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(autokeydown PRIVATE user32 gdi32 shell32)
if(MSVC)
  target_compile_options(autokeydown PRIVATE /W4 /permissive-)
endif()
