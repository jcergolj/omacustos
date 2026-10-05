QT += core testlib
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += \
    backupjob_test.cpp \
    ../src/backupjob.cpp \
    ../src/backupengine.cpp \
    ../src/backupmanifest.cpp \
    ../src/localprovider.cpp

HEADERS += \
    ../src/backupjob.h \
    ../src/backupengine.h \
    ../src/backupprovider.h \
    ../src/localprovider.h
