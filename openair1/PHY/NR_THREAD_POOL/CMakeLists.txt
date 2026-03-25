# NR Thread Pool CMakeLists.txt
# High-performance optimized thread pool for 5G NR signal processing

# Thread pool source files (optimized core only)
set(NR_THREAD_POOL_SOURCES
    nr_thread_pool.c
    nr_thread_pool_init.c
)

# Thread pool header files
set(NR_THREAD_POOL_HEADERS
    nr_thread_pool.h
    nr_thread_pool_init.h
)

# Create static library for thread pool
add_library(nr_thread_pool STATIC ${NR_THREAD_POOL_SOURCES})

# Include directories
target_include_directories(nr_thread_pool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Link required libraries
target_link_libraries(nr_thread_pool
    pthread
    rt
    m  # Math library for optimizations
)

# Thread pool enable/disable option
option(ENABLE_NR_THREAD_POOL "Enable NR Thread Pool parallel processing" ON)

# Compiler flags for optimization
target_compile_options(nr_thread_pool PRIVATE
    -Wall
    -Wextra
    -std=c11
    -O2
    -g
)

# Configure thread pool enable/disable
if(ENABLE_NR_THREAD_POOL)
    target_compile_definitions(nr_thread_pool PUBLIC NR_THREAD_POOL_ENABLE=1)
    message(STATUS "NR Thread Pool: ENABLED")
else()
    target_compile_definitions(nr_thread_pool PUBLIC NR_THREAD_POOL_ENABLE=0)
    message(STATUS "NR Thread Pool: DISABLED")
endif()

# Test executable (optional, for verification)
if(BUILD_TESTING OR BUILD_NR_THREAD_POOL_TESTS)
    add_executable(test_nr_thread_pool test_nr_thread_pool.c)
    target_link_libraries(test_nr_thread_pool nr_thread_pool)

    # Add test to CTest if enabled
    if(BUILD_TESTING)
        add_test(NAME nr_thread_pool_test COMMAND test_nr_thread_pool 4)
    endif()
endif()

# Install headers for other components to use
install(FILES ${NR_THREAD_POOL_HEADERS}
    DESTINATION include/openair1/PHY/NR_THREAD_POOL
)

# Export target for other CMake projects
set_target_properties(nr_thread_pool PROPERTIES
    EXPORT_NAME NR_THREAD_POOL
)