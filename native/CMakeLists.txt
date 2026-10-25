cmake_minimum_required(VERSION 3.21)

project(omacustos VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 QuickDialogs2 Test Concurrent QuickTest)
find_package(LibArchive REQUIRED)
find_package(ZLIB REQUIRED)

# Storage, execution, and configuration are usable without a GUI or QML engine.
add_library(omacustos-core STATIC
    src/backupengine.cpp
    src/batchedbackup.cpp
    src/backuparchive.cpp
    src/backupcontinuation.cpp
    src/backupstaging.cpp
    src/backupmanifest.cpp
    src/incrementalmanifest.cpp
    src/incrementalplanner.cpp
    src/incrementalrepository.cpp
    src/incrementalworker.cpp
    src/incrementalrestore.cpp
    src/backupcatalog.cpp
    src/payloadmetadatapolicy.h
    src/remotemetadatacache.h
    src/backupecleanup.cpp
    src/backupconfig.cpp
    src/settingsarchive.cpp
    src/backupschedule.cpp
    src/backuprunstore.cpp
    src/backupprerequisites.cpp
    src/localprovider.cpp
    src/protonprovider.cpp
    src/protonfolderlink.cpp
    src/qprocessrunner.cpp
    src/backupjob.cpp
    src/backupworker.cpp
    src/workerexecution.cpp
    src/systemdlauncher.cpp
    src/serviceinstaller.cpp
)
target_link_libraries(omacustos-core PUBLIC Qt6::Core LibArchive::LibArchive ZLIB::ZLIB)
target_include_directories(omacustos-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# UI-facing controllers add asynchronous work, but no GUI-only dependencies.
add_library(omacustos-controllers STATIC
    src/backupsetcontroller.cpp
    src/cloudsettingscontroller.cpp
    src/backuprestorecontroller.cpp
    src/backuplauncher.cpp
    src/backupscheduler.cpp
    src/protonfolderbrowser.cpp
    src/recentbackupcopies.cpp
    src/incrementalmaintenance.cpp
    src/protonauthcontroller.cpp
    src/resourceusage.cpp
)
target_link_libraries(omacustos-controllers PUBLIC omacustos-core Qt6::Concurrent)
qt_add_resources(omacustos-controllers backup_set_template
    PREFIX "/omacustos"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/.."
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../backup-sets.template.json"
)

add_library(omacustos-theme STATIC src/themecolors.cpp)
target_link_libraries(omacustos-theme PUBLIC Qt6::Gui)
target_include_directories(omacustos-theme PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

qt_add_executable(omacustos src/main.cpp)
target_link_libraries(omacustos PRIVATE
    omacustos-controllers
    omacustos-theme
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
)

qt_add_executable(omacustos-worker src/worker_main.cpp)
target_link_libraries(omacustos-worker PRIVATE omacustos-core)

# Advanced direct-request interface; the normal worker uses the same executor.
qt_add_executable(omacustos-incremental src/incremental_main.cpp)
target_link_libraries(omacustos-incremental PRIVATE omacustos-core)

qt_add_executable(omacustos-install src/install_main.cpp)
target_link_libraries(omacustos-install PRIVATE omacustos-core)

qt_add_qml_module(omacustos
    URI OmaCustos
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        qml/Main.qml
        qml/Dashboard.qml
        qml/BackupSetCard.qml
        qml/BackupEditor.qml
        qml/RestorePanel.qml
        qml/RestoreProgressPanel.qml
        qml/RestoreSelection.qml
        qml/ActionButton.qml
        qml/ActionDialog.qml
        qml/FormCard.qml
        qml/TransferProgress.qml
        qml/PreviewGroup.qml
        qml/UiStyle.qml
        qml/LocalPaths.js
)

enable_testing()

# Each test links the same implementation target used by the application.
function(omacustos_add_test name source library)
    qt_add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${library} Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

omacustos_add_test(qprocessrunner-test tests/qprocessrunner_test.cpp omacustos-core)
set_tests_properties(qprocessrunner-test PROPERTIES TIMEOUT 30)

omacustos_add_test(backupscheduler-test tests/backupscheduler_test.cpp omacustos-controllers)
omacustos_add_test(backuplauncher-test tests/backuplauncher_test.cpp omacustos-controllers)
qt_add_executable(worker-prerequisite-fixture tests/workerprerequisitefixture.cpp)
target_link_libraries(worker-prerequisite-fixture PRIVATE omacustos-core)
target_compile_definitions(backuplauncher-test PRIVATE
    OMACUSTOS_WORKER_BINARY="$<TARGET_FILE:omacustos-worker>"
    OMACUSTOS_WAITING_WORKER_BINARY="$<TARGET_FILE:worker-prerequisite-fixture>")
add_dependencies(backuplauncher-test omacustos-worker worker-prerequisite-fixture)
set_tests_properties(backuplauncher-test PROPERTIES TIMEOUT 60)
omacustos_add_test(resourceusage-test tests/resourceusage_test.cpp omacustos-controllers)
omacustos_add_test(protonauthcontroller-test tests/protonauthcontroller_test.cpp omacustos-controllers)
omacustos_add_test(themecolors-test tests/themecolors_test.cpp omacustos-theme)
set_tests_properties(themecolors-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=")

omacustos_add_test(backupsetcontroller-test tests/backupsetcontroller_test.cpp omacustos-controllers)
omacustos_add_test(settingsarchive-test tests/settingsarchive_test.cpp omacustos-controllers)
target_link_libraries(backupsetcontroller-test PRIVATE Qt6::Qml)
omacustos_add_test(protonfolderbrowser-test tests/protonfolderbrowser_test.cpp omacustos-controllers)
omacustos_add_test(recentbackupcopies-test tests/recentbackupcopies_test.cpp omacustos-controllers)

qt_add_executable(dashboard-test tests/dashboard_test.cpp)
target_link_libraries(dashboard-test PRIVATE Qt6::QuickTest Qt6::QuickControls2 Qt6::QuickDialogs2)
target_compile_definitions(dashboard-test PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_test(NAME dashboard-test COMMAND dashboard-test)
set_tests_properties(dashboard-test PROPERTIES
    # Whole-suite correctness budget: allow headroom over the observed ~82 s run
    # while retaining bounded hang detection (not a responsiveness threshold).
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Fusion;QT_QPA_PLATFORMTHEME=;QT_FORCE_STDERR_LOGGING=1"
)

omacustos_add_test(backupengine-test tests/backupengine_test.cpp omacustos-core)
omacustos_add_test(scanerrors-test tests/scanerrors_test.cpp omacustos-core)
target_link_libraries(scanerrors-test PRIVATE dl)
omacustos_add_test(protonprovider-test tests/protonprovider_test.cpp omacustos-core)

omacustos_add_test(protonclifixture-test tests/protonclifixture_test.cpp omacustos-core)

omacustos_add_test(backupjob-test tests/backupjob_test.cpp omacustos-core)
omacustos_add_test(backupworker-test tests/backupworker_test.cpp omacustos-core)
omacustos_add_test(systemdlauncher-test tests/systemdlauncher_test.cpp omacustos-core)
omacustos_add_test(serviceinstaller-test tests/serviceinstaller_test.cpp omacustos-core)

omacustos_add_test(reliability-test tests/reliability_test.cpp omacustos-core)
omacustos_add_test(localstate-store-test tests/localstate_store_test.cpp omacustos-core)
target_compile_definitions(reliability-test PRIVATE OMACUSTOS_WORKER_BINARY="$<TARGET_FILE:omacustos-worker>")
add_dependencies(reliability-test omacustos-worker)

qt_add_executable(workerrecovery-test tests/workerrecovery_test.cpp)
target_link_libraries(workerrecovery-test PRIVATE omacustos-controllers Qt6::Test)
# Split the existing data-heavy suite instead of increasing its hang timeout or
# dropping cases. Every test function remains registered exactly once.
add_test(NAME workerrecovery-test COMMAND workerrecovery-test
    workerAppliesIncludeOnlyRules archiveRolloutDefaults recoveryPreservesTheLogicalRun
    terminatedWorkerRecovers boundedBatches rescanReusesUnchangedAndReconcilesEdits
    controlsStopTransfersAndSurviveScheduler storageWaitingPreservesCheckpoint
    stagingRecoveryProtectsActiveAndUnrelatedData stagingRejectsUnsafeLocations
    uncheckpointedUploadsAndRemoteDamageAreReconciled namespaceChangesRemainRestorable
    prerequisiteWaitingCanBeCancelledWithoutTouchingHistoricalCopies
    sourceFailuresFinalizeIncompleteAndProtectSuccessfulCopies archiveSourceFailuresWithoutSurvivors)
add_test(NAME workerarchive-test COMMAND workerrecovery-test
    singleArchiveRoundTrip singleArchiveRejectsDamage singleArchivePreparationRace
    singleArchiveStorageFailure singleArchiveVerificationFailure boundedArchivesRoundTrip
    boundedArchivesStorageWaiting archiveInterruptionRecovery archiveReuseStorageWaiting archiveReconciliation)
add_library(staging-quota-fixture SHARED tests/staging_quota_fixture.cpp)
target_link_libraries(staging-quota-fixture PRIVATE dl)
add_library(staging-observer SHARED tests/staging_observer.cpp)
target_link_libraries(staging-observer PRIVATE dl)
omacustos_add_test(stagingobserver-test tests/stagingobserver_test.cpp omacustos-core)
target_link_libraries(stagingobserver-test PRIVATE staging-observer)
set_tests_properties(stagingobserver-test PROPERTIES TIMEOUT 30 ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:staging-observer>")
target_compile_definitions(workerrecovery-test PRIVATE
    OMACUSTOS_WORKER_BINARY="$<TARGET_FILE:omacustos-worker>"
    OMACUSTOS_CONTINUATION_CLI="${CMAKE_CURRENT_SOURCE_DIR}/tests/continuation_cli.py"
    OMACUSTOS_QUOTA_FIXTURE="$<TARGET_FILE:staging-quota-fixture>")
add_dependencies(workerrecovery-test omacustos-worker staging-quota-fixture)
set_tests_properties(workerrecovery-test workerarchive-test PROPERTIES TIMEOUT 180)

omacustos_add_test(backupconfig-test tests/backupconfig_test.cpp omacustos-core)
omacustos_add_test(backupmanifest-test tests/backupmanifest_test.cpp omacustos-core)
omacustos_add_test(incrementalmanifest-test tests/incrementalmanifest_test.cpp omacustos-core)
omacustos_add_test(incrementalrestore-test tests/incrementalrestore_test.cpp omacustos-core)
target_compile_definitions(incrementalmanifest-test PRIVATE OMACUSTOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
omacustos_add_test(incrementalplanner-test tests/incrementalplanner_test.cpp omacustos-core)
omacustos_add_test(incrementalrepository-test tests/incrementalrepository_test.cpp omacustos-core)
qt_add_executable(incrementalworker-test tests/incrementalworker_test.cpp)
target_link_libraries(incrementalworker-test PRIVATE omacustos-controllers Qt6::Test)
add_test(NAME incrementalworker-test COMMAND incrementalworker-test
    baselineUnchangedEditAndRestore incompleteScanDoesNotPublish
    skippedLinksAndSpecialNodesDoNotInvalidateRegularFiles copiedImportedAndLostStateRequireTakeover
    copiedAndUnboundConfigurationIsReadOnly explicitEnrollmentOfVerifiedEmptySet
    configurationSourcesDoNotCaptureTheirOwnCoordinationState
    deferredCleanupDoesNotBlockScheduledWrites authorizedCleanupRecoversWithoutExpandingScope
    mixedFormatUpgradeAndExactRetention)
add_test(NAME incrementalworker-process-test COMMAND incrementalworker-test
    normalWorkerUsesIncrementalByDefault workerPauseCancelAndResume
    publishedManifestSurvivesLostResponseAndChangedSources defaultWorkerResumesLegacyBeforeFreshBaseline)
add_test(NAME incrementalworker-failures-test COMMAND incrementalworker-test
    workerUploadFailureCannotPublish workerRejectsUnreadableRetainedManifest sourceMutationDuringCaptureCannotPublish)
add_test(NAME incrementalworker-integrity-test COMMAND incrementalworker-test
    missingAndSameSizeCorruptionThroughWorker)
add_test(NAME incrementalownership-test COMMAND incrementalworker-test
    settingsLossAndCopiedConfigurationNeedControllerTakeover takeoverPreservesConcurrentConfiguration)
add_test(NAME incrementalmaintenance-test COMMAND incrementalworker-test
    cleanupControllerRequiresConfirmationAndRestoresRetainedState integrityActivityClearsAfterFailure)
add_test(NAME incrementalcleanup-recovery-test COMMAND incrementalworker-test
    protectedWriterAndInterruptedAuthorizedControllerCleanupRecover)
target_compile_definitions(incrementalworker-test PRIVATE
    OMACUSTOS_WORKER_BINARY="$<TARGET_FILE:omacustos-worker>"
    OMACUSTOS_QUOTA_FIXTURE="$<TARGET_FILE:staging-quota-fixture>"
    OMACUSTOS_CONTINUATION_CLI="${CMAKE_CURRENT_SOURCE_DIR}/tests/continuation_cli.py")
add_dependencies(incrementalworker-test omacustos-worker staging-quota-fixture)
set_tests_properties(incrementalworker-test incrementalworker-process-test incrementalworker-failures-test incrementalworker-integrity-test incrementalownership-test incrementalmaintenance-test incrementalcleanup-recovery-test PROPERTIES TIMEOUT 180)
omacustos_add_test(backuprestorecontroller-test tests/backuprestorecontroller_test.cpp omacustos-controllers)
target_link_libraries(backuprestorecontroller-test PRIVATE Qt6::Qml)
omacustos_add_test(retention-discovery-test tests/retention_discovery_test.cpp omacustos-controllers)

# Environment-sensitive, on-screen timings are opt-in, never a CTest gate.
qt_add_executable(responsiveness-benchmark tests/responsiveness_benchmark.cpp)
target_link_libraries(responsiveness-benchmark PRIVATE omacustos-controllers Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::Test)
target_compile_definitions(responsiveness-benchmark PRIVATE OMACUSTOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

qt_add_executable(manifest-benchmark tests/manifest_benchmark.cpp)
target_link_libraries(manifest-benchmark PRIVATE omacustos-core)

# Local-only sizing evidence; a tiny correctness smoke run is not a timing gate.
qt_add_executable(incremental-benchmark tests/incremental_benchmark.cpp)
target_link_libraries(incremental-benchmark PRIVATE omacustos-core dl)
target_compile_definitions(incremental-benchmark PRIVATE OMACUSTOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME incremental-benchmark-smoke COMMAND incremental-benchmark 262144 8 65536 documents-code)
add_test(NAME incremental-benchmark-mixed-smoke COMMAND incremental-benchmark 262144 8 65536 mixed)
add_test(NAME incremental-benchmark-observer-smoke COMMAND incremental-benchmark 262144 8 65536 production-mixed)
set_tests_properties(incremental-benchmark-observer-smoke PROPERTIES
    TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:staging-observer>;OMACUSTOS_REQUIRE_STAGING_OBSERVER=1")
set_tests_properties(incremental-benchmark-smoke incremental-benchmark-mixed-smoke PROPERTIES
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

qt_add_executable(largebackup-benchmark tests/largebackup_benchmark.cpp tests/archivebenchmark.cpp)
target_link_libraries(largebackup-benchmark PRIVATE omacustos-core)
