cmake_minimum_required(VERSION 3.16)
project(dirlocker VERSION 2.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(DIRLOCKER_VERSION_STR="${PROJECT_VERSION}")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Werror=return-type)
endif()

add_library(dirlocker_core STATIC
    src/core/crypto.cpp
    src/core/json_mini.cpp
    src/core/path_guard.cpp
    src/core/security_util.cpp
    src/core/state_store.cpp
)
target_include_directories(dirlocker_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(dirlocker_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    enable_language(RC)
    target_link_libraries(dirlocker_core PUBLIC bcrypt)

    add_executable(dirlocker WIN32
        src/windows/acl_locker.cpp
        src/windows/win_registry.cpp
        src/windows/win_dialogs.cpp
        src/windows/main.cpp
        src/windows/app.rc
    )
    target_compile_definitions(dirlocker PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    target_link_libraries(dirlocker PRIVATE
        dirlocker_core
        advapi32
        shell32
        ole32
        oleaut32
        uuid
        comctl32
        user32
        gdi32
        gdiplus
    )
    if(MINGW)
        # Avoid binutils 2.34 ld segfault when combining --dynamicbase with CMake's --out-implib on plain EXEs.
        set(CMAKE_CXX_LINK_EXECUTABLE
            "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
        target_link_options(dirlocker PRIVATE
            -s -static -static-libgcc -static-libstdc++ -municode
            -Wl,--dynamicbase -Wl,--nxcompat)
    endif()

    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE dirlocker_core)
    if(MINGW)
        target_link_options(test_core PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(Threads REQUIRED)
    find_package(Qt5 REQUIRED COMPONENTS Widgets Network X11Extras)

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)

    # 1. Root daemon: dirlockerd
    add_executable(dirlockerd
        src/linux/daemon/main.cpp
    )
    target_link_libraries(dirlockerd PRIVATE dirlocker_core Threads::Threads)

    # 2. Desktop Qt5 GUI & Tray: dirlocker
    add_executable(dirlocker
        src/linux/gui/daemon_client.cpp
        src/linux/gui/idle_monitor.cpp
        src/linux/gui/password_dialog.cpp
        src/linux/gui/main_window.cpp
        src/linux/gui/main.cpp
        src/linux/resources.qrc
    )
    target_link_libraries(dirlocker PRIVATE
        dirlocker_core
        Qt5::Widgets
        Qt5::Network
        Qt5::X11Extras
        X11
        Xss
    )

    # 3. Peony context-menu plugin (pure Qt5 + header-only MenuPluginInterface)
    if(EXISTS "/usr/include/peony-qt/menu-plugin-iface.h")
        add_library(dirlocker-peony-plugin SHARED
            src/linux/gui/daemon_client.cpp
            src/linux/peony-plugin/dirlocker_peony_plugin.cpp
            src/linux/peony-plugin/dirlocker_peony_plugin.hpp
            src/linux/resources.qrc
        )
        target_include_directories(dirlocker-peony-plugin PRIVATE
            /usr/include/peony-qt
            /usr/include/peony-qt/fileop
            /usr/include/peony-qt/model
        )
        target_link_libraries(dirlocker-peony-plugin PRIVATE
            dirlocker_core
            Qt5::Widgets
        )
    endif()

    # 4. Unit tests
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE dirlocker_core)
    add_test(NAME test_core COMMAND test_core)
endif()

