cmake_minimum_required(VERSION 3.20)
project(snatcher_engine C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Musashi 68000 core: m68kmake generates m68kops.[ch] from m68k_in.c at build time.
set(MUSASHI ${CMAKE_CURRENT_SOURCE_DIR}/third_party/musashi)
set(MUSASHI_GEN ${CMAKE_CURRENT_BINARY_DIR}/musashi_gen)
file(MAKE_DIRECTORY ${MUSASHI_GEN})
add_executable(m68kmake ${MUSASHI}/m68kmake.c)
set_target_properties(m68kmake PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${MUSASHI_GEN})
add_custom_command(
  OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
  COMMAND m68kmake ${MUSASHI_GEN} ${MUSASHI}/m68k_in.c
  DEPENDS m68kmake ${MUSASHI}/m68k_in.c
  WORKING_DIRECTORY ${MUSASHI_GEN})
add_library(musashi STATIC
  ${MUSASHI}/m68kcpu.c ${MUSASHI_GEN}/m68kops.c ${MUSASHI}/softfloat/softfloat.c)
target_include_directories(musashi PUBLIC ${MUSASHI} PRIVATE ${MUSASHI_GEN})
target_compile_options(musashi PRIVATE
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)

add_library(nuked_opn2 STATIC third_party/nuked-opn2/ym3438.c)
target_include_directories(nuked_opn2 PUBLIC third_party/nuked-opn2)

# ---- Core (no platform dependencies)
add_library(scdcore STATIC
  src/scd/disc.cpp src/scd/vdp.cpp src/scd/pcm.cpp src/scd/psg.cpp src/scd/system.cpp)
target_include_directories(scdcore PUBLIC src/scd)
target_link_libraries(scdcore PUBLIC musashi nuked_opn2)

# ---- Headless runner (tests, oracle captures)
add_executable(scd_headless src/frontend/headless.cpp)
target_link_libraries(scd_headless PRIVATE scdcore)

# ---- SDL3 front-end
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_executable(snatcher src/frontend/sdl_main.cpp)
  target_link_libraries(snatcher PRIVATE scdcore SDL3::SDL3)
else()
  message(STATUS "SDL3 not found: the 'snatcher' front-end is not built")
endif()
