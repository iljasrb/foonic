cmake_minimum_required(VERSION 3.21)

project(foonic VERSION 0.8.1 LANGUAGES CXX)

find_package(Fooyin REQUIRED)

create_fooyin_plugin(
    foonic
    DEPENDS Fooyin::Core
            Fooyin::Gui
    SOURCES src/core/subsonictrack.h
            src/settings/subsonicsettings.h
            src/settings/subsonicsettingspage.cpp
            src/settings/subsonicsettingspage.h
            src/client/subsonicclient.cpp
            src/client/subsonicclient.h
            src/playback/subsonicplaybackcontroller.cpp
            src/playback/subsonicplaybackcontroller.h
            src/playback/subsonicbufferbar.cpp
            src/playback/subsonicbufferbar.h
            src/playback/subsonicremoteplayback.cpp
            src/playback/subsonicremoteplayback.h
            src/playback/subsonicstreamcache.h
            src/playback/subsonicstreamutils.h
            src/library/subsoniclibrarydownloader.cpp
            src/library/subsoniclibrarydownloader.h
            src/browser/subsonicbrowser.cpp
            src/browser/subsonicbrowser.h
            src/browser/subsonicbrowserwidget.cpp
            src/browser/subsonicbrowserwidget.h
            src/browser/subsonicdialog.cpp
            src/browser/subsonicdialog.h
            src/plugin/subsonicplugin.cpp
            src/plugin/subsonicplugin.h
)

# Let every source include its siblings by bare filename (e.g. "subsonicclient.h").
target_include_directories(
    foonic
    PRIVATE src/core
            src/settings
            src/client
            src/playback
            src/library
            src/browser
            src/plugin
)

# Optional self-check for the pure parsing helpers (off by default).
option(FOONIC_BUILD_TESTS "Build foonic tests" OFF)
if(FOONIC_BUILD_TESTS)
    enable_testing()

    add_executable(foonic_streamutils_test tests/subsonicstreamutils_test.cpp)
    target_link_libraries(foonic_streamutils_test PRIVATE Fooyin::Core)
    target_include_directories(foonic_streamutils_test PRIVATE src/playback)
    add_test(NAME subsonic_streamutils COMMAND foonic_streamutils_test)

    add_executable(foonic_streamcache_test tests/subsonicstreamcache_test.cpp)
    target_link_libraries(foonic_streamcache_test PRIVATE Fooyin::Core)
    target_include_directories(foonic_streamcache_test PRIVATE src/playback)
    add_test(NAME subsonic_streamcache COMMAND foonic_streamcache_test)

    add_executable(foonic_streamdevice_test
        tests/subsonicstreamdevice_test.cpp
        tests/subsoniclogging.cpp
        src/playback/subsonicbufferbar.cpp
        src/playback/subsonicbufferbar.h)
    set_target_properties(foonic_streamdevice_test PROPERTIES AUTOMOC ON)
    target_link_libraries(foonic_streamdevice_test PRIVATE Fooyin::Core Fooyin::Gui)
    target_include_directories(foonic_streamdevice_test PRIVATE src/playback src/settings src/client src/core)
    add_test(NAME subsonic_streamdevice COMMAND foonic_streamdevice_test)
endif()
