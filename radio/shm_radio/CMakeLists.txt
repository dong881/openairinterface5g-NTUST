add_library(shm_radio MODULE shm_radio.c noise_device.c)
set_target_properties(shm_radio PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(shm_radio PRIVATE SIMU shm_td_iq_channel actor)
add_dependencies(shm_radio generate_T)
