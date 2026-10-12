cmake_minimum_required(VERSION 3.21)

project(foosonic VERSION 0.2.0 LANGUAGES CXX)

find_package(Fooyin REQUIRED)

create_fooyin_plugin(
    foosonic
    DEPENDS Fooyin::Core
            Fooyin::Gui
    SOURCES subsonicbrowser.cpp
            subsonicbrowser.h
            subsonicbrowserwidget.cpp
            subsonicbrowserwidget.h
            subsonicclient.cpp
            subsonicclient.h
            subsonicdialog.cpp
            subsonicdialog.h
            subsonicdownloadmanager.cpp
            subsonicdownloadmanager.h
            subsonicsettings.h
            subsonicsettingspage.cpp
            subsonicsettingspage.h
            subsonictrack.h
            subsonicplugin.cpp
            subsonicplugin.h
)
