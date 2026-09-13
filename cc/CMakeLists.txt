cmake_minimum_required(VERSION 3.16)
project(hwacha_sim CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_library(hwsim STATIC
  src/sim.cc src/mem.cc src/isa.cc src/hwacha.cc)
target_include_directories(hwsim PUBLIC src)

add_executable(hwacha-sim src/main.cc)
target_link_libraries(hwacha-sim hwsim)

add_executable(memtest tests/memtest.cc)
target_link_libraries(memtest hwsim)

enable_testing()
add_test(NAME memtest COMMAND memtest)
add_test(NAME kernels COMMAND ${CMAKE_SOURCE_DIR}/tests/run_kernels.sh $<TARGET_FILE:hwacha-sim> ${CMAKE_SOURCE_DIR}/..)
