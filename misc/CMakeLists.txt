project(xrResources)

install(DIRECTORY linux/bash-completion DESTINATION ${CMAKE_INSTALL_DATAROOTDIR})

install(FILES linux/grizzly_cop.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES linux/grizzly_cs.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES linux/grizzly_soc.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

install(FILES media/icons/16x16/grizzly_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/16x16/apps)
install(FILES media/icons/32x32/grizzly_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/32x32/apps)
install(FILES media/icons/48x48/grizzly_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/48x48/apps)
install(FILES media/icons/64x64/grizzly_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps)
install(FILES media/icons/64x64/grizzly_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pixmaps)

install(FILES media/icons/16x16/grizzly_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/16x16/apps)
install(FILES media/icons/32x32/grizzly_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/32x32/apps)
install(FILES media/icons/48x48/grizzly_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/48x48/apps)
install(FILES media/icons/64x64/grizzly_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps)
install(FILES media/icons/64x64/grizzly_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pixmaps)

install(FILES media/icons/16x16/grizzly_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/16x16/apps)
install(FILES media/icons/32x32/grizzly_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/32x32/apps)
install(FILES media/icons/48x48/grizzly_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/48x48/apps)
install(FILES media/icons/64x64/grizzly_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps)
install(FILES media/icons/64x64/grizzly_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pixmaps)
