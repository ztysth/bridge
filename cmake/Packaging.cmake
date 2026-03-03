# Ubuntu packages use Qt's distribution packages; archives use Qt deployment above.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME bridge)
set(CPACK_PACKAGE_VENDOR bridge)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "https://github.com/ztysth/bridge/issues")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local file and folder transfers")
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/ztysth/bridge")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "qml6-module-qtquick, qml6-module-qtquick-window, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-dialogs, qml6-module-qtquick-templates, qml6-module-qtqml, qml6-module-qtqml-models, qml6-module-qtqml-workerscript, qml6-module-qt-labs-folderlistmodel")
include(CPack)
