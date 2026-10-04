set(CMAKE_AUTORCC ON)

add_definitions(-DTRANSLATION_DOMAIN="kwin_effects_stageanim")

set(stageanim_SOURCES
    stageanim.cpp
    main.cpp
    shaders.qrc
)

add_library(stageanim62 MODULE ${stageanim_SOURCES})
target_link_libraries(stageanim62 PRIVATE
    KF6::ConfigGui
    KWin::kwin
)
install(TARGETS stageanim62 DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins)
