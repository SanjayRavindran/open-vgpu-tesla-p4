cmake_minimum_required(VERSION 3.16)
set(GRIDD_PACKAGE_VERSION "1.0.0" CACHE STRING "Package version for CPack")
project(nvidia_gridd_cleanroom VERSION ${GRIDD_PACKAGE_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

set(GRIDD_LIB_SOURCES
    src/log.c
    src/daemon.c
    src/config.c
    src/json_util.c
    src/crypto.c
    src/rm_client.c
    src/token.c
    src/env_fingerprint.c
    src/trusted_store.c
    src/http_client.c
    src/nls_client.c
    src/license_manager.c
    src/csp_platform.c
    src/dbus_server.c
)

add_library(gridd_core STATIC ${GRIDD_LIB_SOURCES})
target_include_directories(gridd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(gridd_core PRIVATE -Wall -Wextra -Werror -Wpedantic)
target_link_libraries(gridd_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})

add_executable(nvidia-gridd src/main.c)
target_compile_options(nvidia-gridd PRIVATE -Wall -Wextra -Werror -Wpedantic)
target_link_libraries(nvidia-gridd PRIVATE gridd_core)

enable_testing()
add_executable(c_unit_tests tests/c_unit_tests.c)
target_compile_options(c_unit_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
target_link_libraries(c_unit_tests PRIVATE gridd_core)
add_test(NAME c_unit_tests COMMAND c_unit_tests)

add_executable(c_e2e_tests tests/c_e2e_tests.c)
target_compile_options(c_e2e_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
target_link_libraries(c_e2e_tests PRIVATE gridd_core)

install(TARGETS nvidia-gridd RUNTIME DESTINATION sbin)
install(FILES packaging/nvidia-gridd.service DESTINATION lib/systemd/system)
install(FILES packaging/gridd.conf.template DESTINATION share/nvidia)

configure_file(packaging/debian/postinst ${CMAKE_CURRENT_BINARY_DIR}/debian/postinst @ONLY NEWLINE_STYLE LF)
configure_file(packaging/debian/prerm ${CMAKE_CURRENT_BINARY_DIR}/debian/prerm @ONLY NEWLINE_STYLE LF)
if(UNIX)
    execute_process(COMMAND chmod 0755
        "${CMAKE_CURRENT_BINARY_DIR}/debian/postinst"
        "${CMAKE_CURRENT_BINARY_DIR}/debian/prerm")
endif()

set(CPACK_PACKAGE_NAME "nvidia-gridd")
set(CPACK_PACKAGE_VENDOR "Clean-Room vGPU Licensing Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Clean-room C11 NVIDIA vGPU / GRID licensing daemon")
set(CPACK_PACKAGE_DESCRIPTION "Clean-room C11 Linux implementation of the NVIDIA vGPU / GRID licensing daemon linking OpenSSL 3.x and runtime libdbus-1.so.3.")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_GENERATOR "DEB;TGZ")

set(CPACK_DEBIAN_PACKAGE_NAME "nvidia-gridd")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Maintainer <maintainer@localhost>")
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_BINARY_DIR}/debian/postinst;${CMAKE_CURRENT_BINARY_DIR}/debian/prerm")

set(CPACK_RPM_PACKAGE_NAME "nvidia-gridd")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
set(CPACK_RPM_PACKAGE_GROUP "System Environment/Daemons")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_BINARY_DIR}/debian/postinst")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_BINARY_DIR}/debian/prerm")

include(CPack)
