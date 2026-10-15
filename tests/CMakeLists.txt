add_executable(init_ipod init_ipod.c)
target_link_libraries(init_ipod PRIVATE libgpod::gpod)

add_test(NAME smoke
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/smoke.sh
                 $<TARGET_FILE:navipod> $<TARGET_FILE:init_ipod> ${CMAKE_CURRENT_SOURCE_DIR}/mock_navidrome.py)
