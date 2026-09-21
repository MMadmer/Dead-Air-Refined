project(xrResources)

install(DIRECTORY linux/bash-completion DESTINATION ${CMAKE_INSTALL_DATAROOTDIR})

install(FILES linux/xfined-ray_cop.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES linux/xfined-ray_cs.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES linux/xfined-ray_soc.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

install(FILES media/icons/16x16/xfined-ray_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/16x16/apps)
install(FILES media/icons/32x32/xfined-ray_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/32x32/apps)
install(FILES media/icons/48x48/xfined-ray_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/48x48/apps)
install(FILES media/icons/64x64/xfined-ray_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps)
install(FILES media/icons/64x64/xfined-ray_cop.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pixmaps)

install(FILES media/icons/16x16/xfined-ray_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/16x16/apps)
install(FILES media/icons/32x32/xfined-ray_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/32x32/apps)
install(FILES media/icons/48x48/xfined-ray_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/48x48/apps)
install(FILES media/icons/64x64/xfined-ray_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps)
install(FILES media/icons/64x64/xfined-ray_cs.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pixmaps)

install(FILES media/icons/16x16/xfined-ray_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/16x16/apps)
install(FILES media/icons/32x32/xfined-ray_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/32x32/apps)
install(FILES media/icons/48x48/xfined-ray_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/48x48/apps)
install(FILES media/icons/64x64/xfined-ray_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps)
install(FILES media/icons/64x64/xfined-ray_soc.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pixmaps)
