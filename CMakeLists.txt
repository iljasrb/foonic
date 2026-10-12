cmake_minimum_required(VERSION 3.21)

project(foosonic VERSION 0.2.0 LANGUAGES CXX)

find_package(Fooyin REQUIRED)

create_fooyin_plugin(
    foosonic
    DEPENDS Fooyin::Core
            Fooyin::Gui
    SOURCES src/core/subsonictrack.h
            src/settings/subsonicsettings.h
            src/settings/subsonicsettingspage.cpp
            src/settings/subsonicsettingspage.h
            src/client/subsonicclient.cpp
            src/client/subsonicclient.h
            src/playback/subsonicdownloadmanager.cpp
            src/playback/subsonicdownloadmanager.h
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
    foosonic
    PRIVATE src/core
            src/settings
            src/client
            src/playback
            src/browser
            src/plugin
)
