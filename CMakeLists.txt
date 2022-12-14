
project(Halide)
cmake_minimum_required(VERSION 3.1.3)

find_package(Threads QUIET)

find_package(LLVM REQUIRED CONFIG)

# Notify the user what paths and LLVM version we are using
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  set(CMAKE_OBJECT_PATH_MAX 260)
  message("Windows: setting CMAKE_OBJECT_PATH_MAX to ${CMAKE_OBJECT_PATH_MAX}")
endif()

# Require C++11 for everything.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MACOSX_RPATH ON)


set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(LLVM_VERSION "${LLVM_VERSION_MAJOR}${LLVM_VERSION_MINOR}")

file(TO_NATIVE_PATH "${LLVM_TOOLS_BINARY_DIR}/llvm-as${CMAKE_EXECUTABLE_SUFFIX}" LLVM_AS)
file(TO_NATIVE_PATH "${LLVM_TOOLS_BINARY_DIR}/llvm-nm${CMAKE_EXECUTABLE_SUFFIX}" LLVM_NM)
file(TO_NATIVE_PATH "${LLVM_TOOLS_BINARY_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX}" CLANG)
file(TO_NATIVE_PATH "${LLVM_TOOLS_BINARY_DIR}/llvm-config${CMAKE_EXECUTABLE_SUFFIX}" LLVM_CONFIG)

# LLVM doesn't appear to expose --system-libs via its CMake interface, 
# so we must shell out to llvm-config to find this info
execute_process(COMMAND ${LLVM_CONFIG} --system-libs OUTPUT_VARIABLE HALIDE_SYSTEM_LIBS)
string(STRIP "${HALIDE_SYSTEM_LIBS}" HALIDE_SYSTEM_LIBS)  # strip whitespace from start & end
string(REPLACE " " ";" HALIDE_SYSTEM_LIBS "${HALIDE_SYSTEM_LIBS}")  # convert into a list
if("${HALIDE_SYSTEM_LIBS}" STREQUAL "")
  # It's theoretically possible that this could be legitimately empty,
  # but in practice that doesn't really happen, so we'll assume it means we
  # aren't configured correctly.
  message(FATAL_ERROR "'llvm-config --system-libs' is empty; this is probably wrong.")
endif()

# Check LLVM
function(check_dir VARNAME PATH)
  if (NOT IS_ABSOLUTE "${PATH}")
    message(FATAL_ERROR "\"${PATH}\" (${VARNAME}) must be an absolute path")
  endif()
  if (NOT IS_DIRECTORY "${PATH}")
    message(FATAL_ERROR "\"${PATH}\" (${VARNAME}) must be a directory")
  endif()
endfunction()
function(check_tool_exists NAME PATH)
  # Need to convert to CMake path so that backslashes don't get
  # interpreted as an escape.
  file(TO_CMAKE_PATH "${PATH}" TOOL_PATH)
  if (NOT EXISTS "${TOOL_PATH}")
    message(FATAL_ERROR "Tool ${NAME} not found at ${TOOL_PATH}")
  endif()
  message(STATUS "Using ${NAME} at ${TOOL_PATH}")
endfunction()

# Check LLVM tools exist
check_tool_exists(llvm-as "${LLVM_AS}")
check_tool_exists(llvm-nm "${LLVM_NM}")
check_tool_exists(clang "${CLANG}")

# Check reported LLVM version
if (NOT "${LLVM_VERSION}" MATCHES "^[0-9][0-9]$")
  message(FATAL_ERROR "LLVM_VERSION not specified correctly. Must be <major><minor> E.g. LLVM 3.9 is \"39\"")
endif()
if (LLVM_VERSION LESS 39)
  message(FATAL_ERROR "LLVM version must be 3.9 or newer")
endif()

function(check_llvm_target TARGET HAS_TARGET)
  set(${HAS_TARGET} OFF PARENT_SCOPE)
  set(_llvm_required_version ${LLVM_VERSION})
  if (ARGV2)
    set(_llvm_required_version ${ARGV2})
  endif()
  if (NOT LLVM_VERSION LESS _llvm_required_version)
    list(FIND LLVM_TARGETS_TO_BUILD ${TARGET} _found_target)
    if (_found_target GREATER -1)
      set(${HAS_TARGET} ON PARENT_SCOPE)
    else()
      set(${HAS_TARGET} OFF PARENT_SCOPE)
    endif()
  endif()
endfunction()

check_llvm_target(X86 WITH_X86)
check_llvm_target(ARM WITH_ARM)
check_llvm_target(AArch64 WITH_AARCH64)
check_llvm_target(Hexagon WITH_HEXAGON 39)
check_llvm_target(Mips WITH_MIPS)
check_llvm_target(PowerPC WITH_POWERPC)
check_llvm_target(NVPTX WITH_NVPTX)

option(TARGET_NATIVE_CLIENT "Include Native Client" OFF)
option(TARGET_X86 "Include x86 target" ${WITH_X86})
option(TARGET_ARM "Include ARM target" ${WITH_ARM})
option(TARGET_AARCH64 "Include AARCH64 (arm64) target" ${WITH_AARCH64})
option(TARGET_HEXAGON "Include Hexagon target" ${WITH_HEXAGON})
option(TARGET_METAL "Include Metal target" ON)
option(TARGET_MIPS "Include MIPS target" ${WITH_MIPS})
option(TARGET_POWERPC "Include POWERPC target" ${WITH_POWERPC})
option(TARGET_PTX "Include PTX target" ${WITH_NVPTX})
option(TARGET_OPENCL "Include OpenCL-C target" ON)
option(TARGET_OPENGL "Include OpenGL/GLSL target" ON)
option(TARGET_OPENGLCOMPUTE "Include OpenGLCompute target" ON)
option(HALIDE_SHARED_LIBRARY "Build as a shared library" ON)

if (HALIDE_SHARED_LIBRARY)
  set(HALIDE_LIBRARY_TYPE SHARED)
else()
  set(HALIDE_LIBRARY_TYPE STATIC)
endif()

function(halide_project name folder)
  add_executable("${name}" ${ARGN})
  if (MSVC)
  else()
    target_compile_options("${name}" PUBLIC "-fno-rtti")
  endif()
  target_compile_definitions("${name}" PRIVATE "-DHalide_${HALIDE_LIBRARY_TYPE}")
  target_link_libraries("${name}" PRIVATE Halide ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
  target_include_directories("${name}" PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_include_directories("${name}" PRIVATE "${CMAKE_SOURCE_DIR}/tools")
  set_target_properties("${name}" PROPERTIES FOLDER "${folder}")
  if (MSVC)
    target_link_libraries("${name}" PRIVATE Kernel32)
  endif()
endfunction(halide_project)

# Set warnings globally
option(WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
if (WARNINGS_AS_ERRORS)
    message(STATUS "WARNINGS_AS_ERRORS enabled")
else()
    message(STATUS "WARNINGS_AS_ERRORS disabled")
endif()

if (NOT MSVC)
    add_compile_options(-Wall
                        -Wno-unused-function
                        -Wcast-qual
                        -Woverloaded-virtual
                        -Wignored-qualifiers)
  if (WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
else()
  add_compile_options(/W3)
  add_compile_options(/wd4018)  # disable "signed/unsigned mismatch"
  add_compile_options(/wd4503)  # disable "decorated name length exceeded, name was truncated"
  add_compile_options(/wd4267)  # disable "conversion from 'size_t' to 'int', possible loss of data"
  if (WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
endif()

# These tools are needed by several subdirectories
add_executable(build_halide_h tools/build_halide_h.cpp)
add_executable(binary2cpp tools/binary2cpp.cpp)
if (MSVC)
  # disable irrelevant "POSIX name" warnings
  target_compile_options(build_halide_h PUBLIC /wd4996)
  target_compile_options(binary2cpp PUBLIC /wd4996)
endif()


# Look for OpenMP
find_package(OpenMP QUIET)
if (OPENMP_FOUND)
  message(STATUS "Found OpenMP")
endif()

# For in-tree builds, we need to set the input variables for halide.cmake
# to specific values, rather than relying on HALIDE_DISTRIB_DIR to be set correctly.
set(HALIDE_INCLUDE_DIR "${CMAKE_BINARY_DIR}/include")
set(HALIDE_TOOLS_DIR "${CMAKE_SOURCE_DIR}/tools")
set(HALIDE_COMPILER_LIB Halide)
set(HALIDE_DISTRIB_DIR "/bad-path")
include(halide.cmake)

# -----------------------------------------------------------------------------
# Option to enable/disable assertions
# -----------------------------------------------------------------------------
# Filter out definition of NDEBUG definition from the default build
# configuration flags.  # We will add this ourselves if we want to disable
# assertions.
# FIXME: Perhaps our own default ``cxx_flags_overrides.cmake`` file would be better?
foreach (build_config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${build_config} upper_case_build_config)
  foreach (language CXX C)
    set(VAR_TO_MODIFY "CMAKE_${language}_FLAGS_${upper_case_build_config}")
    string(REGEX REPLACE "(^| )[/-]D *NDEBUG($| )"
                         " "
                         replacement
                         "${${VAR_TO_MODIFY}}"
          )
    #message("Original (${VAR_TO_MODIFY}) is ${${VAR_TO_MODIFY}} replacement is ${replacement}")
    set(${VAR_TO_MODIFY} "${replacement}" CACHE STRING "Default flags for ${build_config} configuration" FORCE)
  endforeach()
endforeach()

function(define_group GROUP)
  if(TARGET "${GROUP}")
    message(FATAL_ERROR "Group ${GROUP} is already defined.")    
  endif()
  add_custom_target("${GROUP}")
  set_target_properties("${GROUP}" PROPERTIES EXCLUDE_FROM_ALL TRUE)
endfunction()

# Make a target that aggregates a number of other targets;
# this can be used to group builds (e.g. "build_tests")
# or to execute targets (by adding executable custom-commands);
# the latter is used to execute test targets.
#
# Note that the target will be excluded from the "all" target, so it won't
# be built by "make all" by default.
#
# TODO: this is intended to eventually replicate all of the interesting test targets
# from our Make build, but not all are implemented yet:
# TODO(srj): add test_aotcpp_generators support
# TODO(srj): add test_valgrind variant
# TODO(srj): add test_avx512 variant
# TODO(srj): add test_bazel variant
# TODO(srj): add test_python variant
# TODO(srj): add test_apps variant
function(add_to_group GROUP TARGET)
  set(options BUILD EXECUTE)
  set(oneValueArgs WORKING_DIRECTORY)
  set(multiValueArgs )
  cmake_parse_arguments(args "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  if(NOT TARGET "${GROUP}")
    define_group("${GROUP}")
  endif()
  if (${args_EXECUTE})
    # Execute the target (e.g. for a test_foo target)
    set(NAME "_execute_${TARGET}")
    get_target_property(TARGET_TYPE "${TARGET}" TYPE)
    if("${TARGET_TYPE}" STREQUAL "EXECUTABLE")
      add_custom_target("${NAME}" 
                         COMMAND "${TARGET}"
                         WORKING_DIRECTORY ${args_WORKING_DIRECTORY}
                         COMMENT "Executing ${TARGET}"
                         DEPENDS "${TARGET}")
    elseif("${TARGET_TYPE}" STREQUAL "UTILITY")
      # It's probably a custom target or a group: just depend on it.
      add_custom_target("${NAME}" 
                         DEPENDS "${TARGET}")
    else()
      message(FATAL_ERROR "add_to_group(): unsupported type ${TARGET_TYPE} for ${TARGET}")
    endif()
  elseif(${args_BUILD})
    # Just depend on the target (e.g. for a build_foo target)
    set(NAME "_build_${TARGET}")
    add_custom_target("${NAME}" 
                       DEPENDS "${TARGET}")
  else()
    message(FATAL_ERROR "add_to_group() requires BUILD or EXECUTE")
  endif()
  set_target_properties("${NAME}" PROPERTIES EXCLUDE_FROM_ALL TRUE)
  add_dependencies("${GROUP}" "${NAME}")
endfunction()

# Only groups that are defined in *this file* will have projects reliably
# defined for them in MSVC builds; pre-define the targets we want to be available
# here to ensure that happens.
define_group(build_tests)
define_group(test_auto_schedule)
define_group(test_correctness)
define_group(test_error)
define_group(test_generator)
define_group(test_internal)
define_group(test_opengl)
define_group(test_performance)
define_group(test_tutorials)
define_group(test_warning)

define_group(run_tests)
if(WITH_TESTS)
  add_to_group(run_tests test_correctness BUILD)
  add_to_group(run_tests test_error BUILD)
  add_to_group(run_tests test_generators BUILD)
  add_to_group(run_tests test_internal BUILD)
  add_to_group(run_tests test_warning BUILD)
endif()
if(WITH_TUTORIALS)
  add_to_group(run_tests test_tutorials BUILD)
endif()

# TODO(srj): these need to be run one-at-a-time, without any -j option,
# otherwise we can get false slowdown failures.
# add_to_group(run_tests test_performance BUILD)
# add_to_group(run_tests test_auto_schedule BUILD)


add_subdirectory(src)
option(WITH_TESTS "Build tests" ON)
if (WITH_TESTS)
  message(STATUS "Building tests enabled")
  add_subdirectory(test)
else()
  message(STATUS "Building tests disabled")
endif()

option(WITH_APPS "Build apps" OFF)
if (WITH_APPS)
  if (NOT WIN32)
    message(STATUS "Building apps enabled")
    add_subdirectory(apps)
  else()
    message(WARNING "Apps cannot be built under Windows using cmake. Try the makefile in an msys2 shell.")
  endif()
else()
  message(STATUS "Building apps disabled")
endif()
option(WITH_TUTORIALS "Build Tutorials" OFF)
if (WITH_TUTORIALS)
  message(STATUS "Building tutorials enabled")
  add_subdirectory(tutorial)
else()
  message(STATUS "Building tutorials disabled")
endif()

option(WITH_DOCS "Enable building of documentation" OFF)
if (WITH_DOCS)
find_package(Doxygen)
  if (NOT DOXYGEN_FOUND)
    message(FATAL_ERROR "Could not find Doxygen. Either install it or set WITH_DOCS to OFF")
  endif()

  configure_file(${CMAKE_SOURCE_DIR}/Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile @ONLY)
  # Note documentation is not built by default, the user needs to build the "doc" target
  add_custom_target(doc
    COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_BINARY_DIR}/Doxyfile
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Building Doxygen documentation"
  )
endif()

option(WITH_UTILS "Build utils" ON)
if (WITH_UTILS)
  message(STATUS "Building utils enabled")
  add_subdirectory(util)
else()
  message(STATUS "Building utils disabled")
endif()
