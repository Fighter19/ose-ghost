# The ghost hunting game as a game module (see README.md). Linked into every app as an
# object library so its static registration is kept; it only runs with `ghosthunt 1`.
file(GLOB OSE_GHOSTHUNT_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(ose_game_ghosthunt OBJECT ${OSE_GHOSTHUNT_SOURCES})
target_link_libraries(ose_game_ghosthunt PUBLIC ose_engine)
target_compile_definitions(ose_game_ghosthunt PRIVATE OSE_GHOSTHUNT_CONTENT="${CMAKE_CURRENT_SOURCE_DIR}/content")
ose_warnings(ose_game_ghosthunt)
set_property(GLOBAL APPEND PROPERTY OSE_GAME_MODULES ose_game_ghosthunt)

if(OSE_BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED)
    file(GLOB OSE_GHOSTHUNT_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
    # The tests build the module's sources in, so they can reach its internals.
    add_executable(ose_ghosthunt_tests ${CMAKE_SOURCE_DIR}/tests/unit/main.cpp ${OSE_GHOSTHUNT_TEST_SOURCES} ${OSE_GHOSTHUNT_SOURCES})
    target_include_directories(ose_ghosthunt_tests PRIVATE src)
    target_link_libraries(ose_ghosthunt_tests PRIVATE ose_engine doctest::doctest)
    target_compile_definitions(ose_ghosthunt_tests PRIVATE OSE_GHOSTHUNT_CONTENT="${CMAKE_CURRENT_SOURCE_DIR}/content")
    ose_warnings(ose_ghosthunt_tests)
    add_test(NAME unit_ghosthunt COMMAND ose_ghosthunt_tests "--test-case-exclude=*game content*")
    set_tests_properties(unit_ghosthunt PROPERTIES LABELS "unit")
    # With the game installed: every CS:S map's nav file gives rooms and paths.
    add_test(NAME assets_ghosthunt COMMAND ${CMAKE_COMMAND} -E env sh -c
        "if [ -z \"$OSE_CSS_DIR\" ]; then echo 'OSE_CSS_DIR not set, skipping'; exit 77; fi; \
         $<TARGET_FILE:ose_ghosthunt_tests> --test-case='*game content*'")
    set_tests_properties(assets_ghosthunt PROPERTIES LABELS "assets" SKIP_RETURN_CODE 77)
endif()
