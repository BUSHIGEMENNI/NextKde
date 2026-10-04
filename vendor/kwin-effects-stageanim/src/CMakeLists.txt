set(CMAKE_AUTORCC ON)

add_definitions(-DTRANSLATION_DOMAIN="kwin_effects_stageanim")

set(stageanim_SOURCES
    stageanim.cpp
    main.cpp
    shaders.qrc
)

add_library(stageanim52 MODULE ${stageanim_SOURCES})
target_link_libraries(stageanim52 PRIVATE
    KF6::ConfigGui
    KWin::kwin
)
install(TARGETS stageanim52 DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins)
