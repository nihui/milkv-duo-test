project(opencv-mobile-test)
cmake_minimum_required(VERSION 3.5)

set(CMAKE_BUILD_TYPE release)
set(CMAKE_INSTALL_PREFIX install)

# opencv4 requires c++11
set(CMAKE_CXX_STANDARD 11)

include_directories("/home/nihui/osd/duo-buildroot-sdk/middleware/v2/include")
link_directories("/home/nihui/osd/duo-buildroot-sdk/middleware/v2/cv180x/lib_musl_riscv64")

add_executable(testmilkv testmilkv.cpp)
target_link_libraries(testmilkv vdec vpu sys cvi_bin cvi_bin_isp isp isp_algo ae awb libatomic.a)

set_target_properties(testmilkv PROPERTIES INSTALL_RPATH "/mnt/system/lib")

include(GNUInstallDirs)
install(TARGETS testmilkv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
