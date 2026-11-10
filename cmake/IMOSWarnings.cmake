# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# `imos_warnings`: an INTERFACE target carrying the project warning flags and,
# optionally, sanitizer/coverage instrumentation. Linked PRIVATE by our own
# targets only — never by the vendored dependencies.

include_guard(GLOBAL)

add_library(imos_warnings INTERFACE)
add_library(imos::warnings ALIAS imos_warnings)

if(MSVC)
  target_compile_options(imos_warnings INTERFACE /W4 /permissive- /EHsc)
  if(IMOS_WARNINGS_AS_ERRORS)
    target_compile_options(imos_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(imos_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
    -Woverloaded-virtual -Wdouble-promotion -Wformat=2)
  if(IMOS_WARNINGS_AS_ERRORS)
    target_compile_options(imos_warnings INTERFACE -Werror)
  endif()
endif()

# Sanitizers / coverage are propagated through the same interface so every
# first-party target and its tests pick them up together.
if(IMOS_ENABLE_ASAN)
  target_compile_options(imos_warnings INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(imos_warnings INTERFACE -fsanitize=address,undefined)
endif()
if(IMOS_ENABLE_TSAN)
  target_compile_options(imos_warnings INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(imos_warnings INTERFACE -fsanitize=thread)
endif()
if(IMOS_ENABLE_COVERAGE)
  # Atomic counter updates: the transports call into core/ from their I/O
  # threads, and racing plain increments corrupt the counts (gcov then
  # reports negative hits, which gcovr rejects).
  target_compile_options(imos_warnings INTERFACE --coverage -fprofile-update=atomic -O0 -g)
  target_link_options(imos_warnings INTERFACE --coverage)
endif()
