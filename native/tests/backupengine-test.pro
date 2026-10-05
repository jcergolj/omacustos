QT += core testlib
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += \
    backupengine_test.cpp \
    ../src/backupengine.cpp \
    ../src/backupmanifest.cpp \
    ../src/localprovider.cpp

HEADERS += \
    ../src/backupengine.h \
    ../src/backupprovider.h \
    ../src/localprovider.h
