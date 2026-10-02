# Deploy the matching kit's runtime beside each freshly linked application.
win32 {
    QT_DEPLOY_SCRIPT = $$shell_quote($$shell_path($$PWD/deploy_qt_windows.ps1))
    QT_DEPLOY_BIN = $$shell_quote($$shell_path($$[QT_INSTALL_BINS]))
    CONFIG(debug, debug|release): QT_DEPLOY_CONFIGURATION = Debug
    else: QT_DEPLOY_CONFIGURATION = Release
    QMAKE_POST_LINK += powershell.exe -NoProfile -ExecutionPolicy Bypass -File $$QT_DEPLOY_SCRIPT -Binary \"$(DESTDIR_TARGET)\" -QtBin $$QT_DEPLOY_BIN -Configuration $$QT_DEPLOY_CONFIGURATION
}
