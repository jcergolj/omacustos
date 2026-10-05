QT += core testlib
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += \
    backupworker_test.cpp \
    ../src/backupworker.cpp \
    ../src/backupjob.cpp \
    ../src/backupengine.cpp \
    ../src/backupmanifest.cpp \
    ../src/localprovider.cpp

HEADERS += \
    ../src/backupworker.h \
    ../src/backupjob.h \
    ../src/backupengine.h \
    ../src/backupprovider.h \
    ../src/localprovider.h
