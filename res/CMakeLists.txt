project(xrGamedata)

install(FILES fsgame.ltx DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/xfined-ray)

install(DIRECTORY gamedata DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/xfined-ray)
