# ISIP Thread Pool Static Library for OAI 5G
# Builds a tiny static library from enkiTS sources

# Build ISIP pool static library
add_library(isip_pool STATIC
    ${CMAKE_SOURCE_DIR}/external/enkiTS/src/TaskScheduler.cpp
    ${CMAKE_SOURCE_DIR}/external/enkiTS/src/TaskScheduler_c.cpp
    isip_pool.c
)

# Include directories
target_include_directories(isip_pool
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${CMAKE_SOURCE_DIR}/external/enkiTS/src  # Expose enkiTS C API for task parallelization
)

# Link dependencies
find_package(Threads REQUIRED)
target_link_libraries(isip_pool PRIVATE Threads::Threads)

# C++11 requirement for enkiTS
target_compile_features(isip_pool PRIVATE cxx_std_11)

# Compiler flags
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(isip_pool PRIVATE -Wall -Wno-unused-parameter)
endif()
