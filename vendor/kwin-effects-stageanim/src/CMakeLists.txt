set(CMAKE_AUTORCC ON)

add_definitions(-DTRANSLATION_DOMAIN="kwin_effects_stageanim")

set(stageanim_SOURCES
    stageanim.cpp
    main.cpp
    shaders.qrc
)

add_library(stageanim45 MODULE ${stageanim_SOURCES})
target_link_libraries(stageanim45 PRIVATE
    KF6::ConfigGui
    KWin::kwin
)
install(TARGETS stageanim45 DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins)
