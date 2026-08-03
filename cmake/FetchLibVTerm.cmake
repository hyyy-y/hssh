# Fetch and build libvterm from source as a static library.
# This is used as a fallback when find_package(libvterm) fails.

include(FetchContent)

FetchContent_Declare(
    libvterm
    GIT_REPOSITORY https://github.com/neovim/libvterm.git
    GIT_TAG        v0.3.3
)

message(STATUS "Fetching libvterm from GitHub (via proxy if configured)...")
FetchContent_Populate(libvterm)

set(VTERM_SOURCE_DIR ${libvterm_SOURCE_DIR})
set(VTERM_BINARY_DIR ${libvterm_BINARY_DIR})

# Generate encoding tables (.inc files) using the bundled Perl scripts.
# Resolve perl to a full path: build environments that don't inherit the
# developer shell (Qt Creator, plain cmd) often lack Git's usr/bin on PATH,
# where the only perl on Windows usually lives.
find_program(VTERM_PERL_EXECUTABLE perl
    HINTS
        "$ENV{ProgramFiles}/Git/usr/bin"
        "C:/Program Files (x86)/Git/usr/bin"
        "$ENV{LOCALAPPDATA}/Git/usr/bin"
        "$ENV{LOCALAPPDATA}/../Git/usr/bin"
        "$ENV{APPDATA}/../Local/Programs/Git/usr/bin"
        "C:/Strawberry/perl/bin"
)
if(NOT VTERM_PERL_EXECUTABLE)
    message(FATAL_ERROR
        "perl is required to generate libvterm encoding tables "
        "(Git for Windows ships one at <Git>/usr/bin/perl.exe, Strawberry "
        "Perl works too). Install perl or add it to PATH and re-run CMake.")
endif()
message(STATUS "libvterm: using perl at ${VTERM_PERL_EXECUTABLE}")

file(GLOB VTERM_TBL_FILES ${VTERM_SOURCE_DIR}/src/encoding/*.tbl)
set(VTERM_GENERATED_INCS)

foreach(tbl_file ${VTERM_TBL_FILES})
    get_filename_component(inc_name ${tbl_file} NAME_WE)
    set(inc_file ${VTERM_SOURCE_DIR}/src/encoding/${inc_name}.inc)
    add_custom_command(
        OUTPUT ${inc_file}
        COMMAND ${VTERM_PERL_EXECUTABLE} -CSD ${VTERM_SOURCE_DIR}/tbl2inc_c.pl ${tbl_file} > ${inc_file}
        DEPENDS ${tbl_file} ${VTERM_SOURCE_DIR}/tbl2inc_c.pl
        COMMENT "Generating ${inc_name}.inc"
        VERBATIM
    )
    list(APPEND VTERM_GENERATED_INCS ${inc_file})
endforeach()

set(VTERM_FULLWIDTH_INC ${VTERM_SOURCE_DIR}/src/fullwidth.inc)
add_custom_command(
    OUTPUT ${VTERM_FULLWIDTH_INC}
    COMMAND ${VTERM_PERL_EXECUTABLE} ${VTERM_SOURCE_DIR}/find-wide-chars.pl > ${VTERM_FULLWIDTH_INC}
    DEPENDS ${VTERM_SOURCE_DIR}/find-wide-chars.pl
    COMMENT "Generating fullwidth.inc"
    VERBATIM
)
list(APPEND VTERM_GENERATED_INCS ${VTERM_FULLWIDTH_INC})

# Build libvterm as a static library by compiling its C sources directly.
add_library(vterm STATIC
    ${VTERM_SOURCE_DIR}/src/encoding.c
    ${VTERM_SOURCE_DIR}/src/keyboard.c
    ${VTERM_SOURCE_DIR}/src/mouse.c
    ${VTERM_SOURCE_DIR}/src/parser.c
    ${VTERM_SOURCE_DIR}/src/pen.c
    ${VTERM_SOURCE_DIR}/src/screen.c
    ${VTERM_SOURCE_DIR}/src/state.c
    ${VTERM_SOURCE_DIR}/src/unicode.c
    ${VTERM_SOURCE_DIR}/src/vterm.c
    ${VTERM_GENERATED_INCS}
)

target_include_directories(vterm PUBLIC ${VTERM_SOURCE_DIR}/include)

if(WIN32)
    # libvterm uses POSIX functions; MinGW provides them but may need defines.
    target_compile_definitions(vterm PRIVATE _POSIX_C_SOURCE=200809L)
endif()

set(libvterm_FOUND TRUE)
set(libvterm_TARGET vterm)
