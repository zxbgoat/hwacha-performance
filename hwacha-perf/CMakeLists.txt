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
  src/sim.cc src/mem.cc src/isa.cc src/trace.cc src/hwacha.cc)
target_include_directories(hwsim PUBLIC src)

add_executable(hwacha-sim src/main.cc)
find_package(ZLIB REQUIRED)
target_link_libraries(hwsim PUBLIC ZLIB::ZLIB)
target_link_libraries(hwacha-sim hwsim)

add_executable(memtest tests/memtest.cc)
target_link_libraries(memtest hwsim)

enable_testing()
add_test(NAME memtest COMMAND memtest)
add_test(NAME kernels COMMAND ${CMAKE_SOURCE_DIR}/tests/run_kernels.sh $<TARGET_FILE:hwacha-sim> ${CMAKE_SOURCE_DIR}/..)
# RTL 校准回归：用 rtl/results 里的 RTL 计时日志检查 C++ 模型——每个内核的 RTL 误差不超过 20%（已知最差的是谓词密集的 hcc_clamp −18%），且相对
# hwacha-perf/tests/calibration_baseline.json 记录的模型周期数漂移不超过 2%（改模型后确认合理再 --update 基线）
add_test(NAME calibrate COMMAND python3 ${CMAKE_SOURCE_DIR}/../scripts/check_calibration.py --max-err 20 --max-drift 2)
