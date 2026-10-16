cmake_minimum_required(VERSION 3.15)
project(px4_ipc_tests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(ipc_test ipc_test.cpp ../src/common/pipe.cpp)
target_compile_features(ipc_test PRIVATE cxx_std_17)
target_include_directories(ipc_test PRIVATE
 "${REPO_ROOT}/macos/src/common" "${REPO_ROOT}/macos/src/include"
 "${REPO_ROOT}/macos/src/DriverHost_PX4" "${REPO_ROOT}/winusb/src/common")
target_link_libraries(ipc_test PRIVATE Threads::Threads)
add_test(NAME ipc_test COMMAND ipc_test)
set_tests_properties(ipc_test PROPERTIES TIMEOUT 15)
