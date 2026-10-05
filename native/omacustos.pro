QT += core gui qml quick quickcontrols2
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += \
    src/main.cpp \
    src/backupengine.cpp \
    src/backupmanifest.cpp \
    src/backuprestorecontroller.cpp \
    src/localprovider.cpp \
    src/qprocessrunner.cpp \
    src/protonprovider.cpp \
    src/backupjob.cpp \
    src/backupworker.cpp \
    src/backuplauncher.cpp \
    src/backupconfig.cpp \
    src/systemdlauncher.cpp

HEADERS += \
    src/backupengine.h \
    src/backupmanifest.h \
    src/backuprestorecontroller.h \
    src/backupprovider.h \
    src/localprovider.h \
    src/processrunner.h \
    src/qprocessrunner.h \
    src/protonprovider.h \
    src/backupjob.h \
    src/backupworker.h \
    src/backuplauncher.h

RESOURCES += qml.qrc

TARGET = omacustos

worker {
    TARGET = omacustos-worker
    SOURCES = \
        src/worker_main.cpp \
        src/backupconfig.cpp \
        src/backupengine.cpp \
        src/backupmanifest.cpp \
        src/protonprovider.cpp \
        src/qprocessrunner.cpp
    HEADERS = \
        src/backupengine.h \
        src/backupconfig.h \
        src/backupprovider.h \
        src/processrunner.h \
        src/protonprovider.h \
        src/qprocessrunner.h
}

installer {
    TARGET = omacustos-install
    SOURCES = \
        src/install_main.cpp \
        src/serviceinstaller.cpp
    HEADERS = \
        src/serviceinstaller.h
}
