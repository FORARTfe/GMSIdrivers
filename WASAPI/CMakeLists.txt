cmake_minimum_required(VERSION 3.20)
project(maestro2em VERSION 0.4.0 LANGUAGES CXX)

# ── WDK/DriverKit toolchain required ────────────────────────────────────────
# Set environment:
#   WDK_DIR = C:\Program Files (x86)\Windows Kits\10
#   Target:   Windows 10 (also runs on XP/Vista/7 with correct VC runtime)
#
# Build:
#   mkdir build && cd build
#   cmake .. -G "Visual Studio 17 2022" -DCMAKE_SYSTEM_NAME=WindowsKernelModeDriver10.0
#   cmake --build . --config Release

set(CMAKE_CXX_STANDARD 17)

# ── Source files ─────────────────────────────────────────────────────────
set(DRIVER_SOURCES
    src/hw.cpp
    src/adapter.cpp
    src/minwave.cpp
    src/mintopo.cpp
    src/tables.cpp
)

# ── Driver target ─────────────────────────────────────────────────────────
add_library(maestro2em SHARED ${DRIVER_SOURCES})

set_target_properties(maestro2em PROPERTIES
    SUFFIX ".sys"
    PREFIX ""
    LINK_FLAGS "/DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /NODEFAULTLIB"
)

# ── Include paths ─────────────────────────────────────────────────────────
target_include_directories(maestro2em PRIVATE
    ${WDK_DIR}/Include/10.0.22621.0/km
    ${WDK_DIR}/Include/10.0.22621.0/shared
    src
)

# ── WDK kernel libraries ──────────────────────────────────────────────────
target_link_libraries(maestro2em PRIVATE
    portcls.lib
    ks.lib
    ksguid.lib
    ntoskrnl.lib
    hal.lib
    wdm.lib
)

# ── Compile definitions ───────────────────────────────────────────────────
target_compile_definitions(maestro2em PRIVATE
    UNICODE
    _UNICODE
    POOL_NX_OPTIN=1     # Enable non-exec pool by default
    NTDDI_VERSION=0x05010300  # XP SP3 minimum
    _WIN32_WINNT=0x0501
)

# ── Compiler flags ────────────────────────────────────────────────────────
target_compile_options(maestro2em PRIVATE
    /kernel        # kernel-mode compilation
    /GF            # string pooling
    /GS-           # disable security checks (not available in kernel)
    /EHs-c-        # no exceptions in kernel mode
    /wd4200        # zero-length array extension
)

# ── INF target (copy to output) ───────────────────────────────────────────
configure_file(inf/maestro2em.inf
    ${CMAKE_BINARY_DIR}/maestro2em.inf COPYONLY)

install(TARGETS maestro2em
    DESTINATION driver)
install(FILES inf/maestro2em.inf
    DESTINATION driver)
