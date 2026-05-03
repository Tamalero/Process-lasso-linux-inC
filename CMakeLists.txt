cmake_minimum_required(VERSION 3.20)
project(process-lasso-qt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ── main application ──────────────────────────────────────────────────────────

set(APP_SOURCES
    src/main.cpp
    src/config.cpp
    src/cputopology.cpp
    src/cpupark.cpp
    src/processinfo.h
    src/probalance.cpp
    src/processmonitor.cpp
    src/ruleengine.cpp
    src/utils.cpp
    src/gui/cpubarwidget.cpp
    src/gui/dialogs.cpp
    src/gui/gamingmodetab.cpp
    src/gui/mainwindow.cpp
    src/gui/probalancetab.cpp
    src/gui/processtablewidget.cpp
    src/gui/ruleseditor.cpp
    src/gui/settingstab.cpp
)

qt_add_executable(process-lasso-qt ${APP_SOURCES})

target_include_directories(process-lasso-qt PRIVATE src)

target_link_libraries(process-lasso-qt
    PRIVATE
        Qt6::Widgets
)

target_compile_options(process-lasso-qt PRIVATE
    -Wall -Wextra -Wpedantic
)

# ── privileged helper ─────────────────────────────────────────────────────────

add_executable(process-lasso-helper helper/main.cpp)

target_compile_options(process-lasso-helper PRIVATE
    -Wall -Wextra -Wpedantic
)

# ── install rules ─────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS process-lasso-qt
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS process-lasso-helper
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ             GROUP_EXECUTE
                WORLD_READ             WORLD_EXECUTE
)

install(FILES packaging/process-lasso.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# ── packaging helper script ───────────────────────────────────────────────────

install(PROGRAMS packaging/install-helper.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/process-lasso-qt
)
