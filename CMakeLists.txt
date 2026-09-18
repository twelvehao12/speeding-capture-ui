cmake_minimum_required(VERSION 3.20)

project(CameraManagerApp
    VERSION 0.1.0
    LANGUAGES CXX
)

set(VIDEO_STREAMS_CONTROLLER_SOURCES
    src/rv1126b/application/VideoStreamsController.cpp
    src/rv1126b/application/VideoStreamsController.h
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Sql Network Test Multimedia MultimediaWidgets)

option(BUILD_RTSP_SPIKE "Build the manual RV1126B RTSP validation tool" ON)

set(RTSP_PLAYER_CORE_SOURCES
    src/rv1126b/infrastructure/video/IMediaPlaybackBackend.h
    src/rv1126b/infrastructure/video/QtMultimediaRtspPlayer.cpp
    src/rv1126b/infrastructure/video/QtMultimediaRtspPlayer.h
    src/rv1126b/ports/IRtspPlayer.h
)

set(RTSP_PLAYER_SOURCES
    ${RTSP_PLAYER_CORE_SOURCES}
    src/rv1126b/infrastructure/video/QtMediaPlaybackBackend.cpp
    src/rv1126b/infrastructure/video/QtMediaPlaybackBackend.h
    src/rv1126b/infrastructure/video/QtMultimediaRtspPlayerFactory.cpp
)

qt_add_executable(CameraManagerApp
    ${VIDEO_STREAMS_CONTROLLER_SOURCES}
    ${RTSP_PLAYER_SOURCES}
    src/main.cpp
    src/services/UiPerformanceMonitor.h
    src/rv1126b/network/FileDownloadExecutor.h
    src/rv1126b/services/CacheFileLease.h
    src/rv1126b/storage/SqliteEventStore.h
    src/device/IDeviceClient.cpp
    src/device/IDeviceClient.h
    src/device/MockDeviceClient.cpp
    src/device/MockDeviceClient.h
    src/models/CaptureRecord.h
    src/models/Device.h
    src/models/DeviceConfig.h
    src/models/DeviceStatus.h
    src/models/OperationLog.h
    src/models/SystemSettings.h
    src/models/table_models/CaptureRecordTableModel.cpp
    src/models/table_models/CaptureRecordTableModel.h
    src/models/table_models/DevicePropertyModel.cpp
    src/models/table_models/DevicePropertyModel.h
    src/models/table_models/DeviceTableModel.cpp
    src/models/table_models/DeviceTableModel.h
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/application/DeviceIntegrationController.cpp
    src/rv1126b/application/DeviceIntegrationController.h
    src/rv1126b/application/DeviceOperationsController.cpp
    src/rv1126b/application/DeviceOperationsController.h
    src/rv1126b/application/EventViewController.cpp
    src/rv1126b/application/EventViewController.h
    src/rv1126b/application/Rv1126bApplicationRuntime.cpp
    src/rv1126b/application/Rv1126bApplicationRuntime.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/ports/ISecretStore.h
    src/rv1126b/network/BoardApiClient.cpp
    src/rv1126b/network/BoardApiClient.h
    src/rv1126b/network/DirectDeviceProbeService.cpp
    src/rv1126b/network/DirectDeviceProbeService.h
    src/rv1126b/network/UdpDeviceDiscoveryService.cpp
    src/rv1126b/network/UdpDeviceDiscoveryService.h
    src/rv1126b/protocol/ApiCodec.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/protocol/BoardApiCodec.cpp
    src/rv1126b/protocol/BoardApiCodec.h
    src/rv1126b/services/DeviceDiscoveryService.h
    src/rv1126b/services/DeviceFleetService.h
    src/rv1126b/services/DeviceSession.h
    src/rv1126b/services/BoardEventSyncService.cpp
    src/rv1126b/services/BoardEventSyncService.h
    src/rv1126b/services/BoardDeviceSession.cpp
    src/rv1126b/services/BoardDeviceSession.h
    src/rv1126b/services/BoardDeviceFleetService.cpp
    src/rv1126b/services/BoardDeviceFleetService.h
    src/rv1126b/services/BoardEvidenceCache.cpp
    src/rv1126b/services/BoardEvidenceCache.h
    src/rv1126b/services/BoardFtpTaskSnapshotService.cpp
    src/rv1126b/services/BoardFtpTaskSnapshotService.h
    src/rv1126b/services/BoardFtpService.cpp
    src/rv1126b/services/BoardFtpService.h
    src/rv1126b/services/EmbeddedFtpReceiveServer.cpp
    src/rv1126b/services/EmbeddedFtpReceiveServer.h
    src/rv1126b/services/EmbeddedFtpReceiveWorker.h
    src/rv1126b/services/EventSyncService.h
    src/rv1126b/services/EvidenceCache.h
    src/rv1126b/services/EvidenceCacheMaintenanceService.cpp
    src/rv1126b/services/EvidenceCacheMaintenanceService.h
    src/rv1126b/services/FtpService.h
    src/rv1126b/services/LocalEvidenceService.cpp
    src/rv1126b/services/LocalEvidenceService.h
    src/rv1126b/security/WindowsCredentialStore.cpp
    src/rv1126b/security/WindowsCredentialStore.h
    src/rv1126b/storage/SqliteEventRepository.cpp
    src/rv1126b/storage/SqliteEventRepository.h
    src/services/DeviceManager.cpp
    src/services/DeviceManager.h
    src/services/CaptureRecordService.cpp
    src/services/CaptureRecordService.h
    src/services/CaptureStorageService.cpp
    src/services/CaptureStorageService.h
    src/services/MaintenanceController.cpp
    src/services/MaintenanceController.h
    src/services/SystemSettingsService.cpp
    src/services/SystemSettingsService.h
    src/ui/DeviceConfigDialog.cpp
    src/ui/DeviceConfigDialog.h
    src/ui/DeviceDiscoveryDialog.cpp
    src/ui/DeviceDiscoveryDialog.h
    src/ui/LivePreviewPanel.cpp
    src/ui/LivePreviewPanel.h
    src/ui/Rv1126bDeviceManagementDialog.cpp
    src/ui/Rv1126bDeviceManagementDialog.h
    src/ui/SystemSettingsDialog.cpp
    src/ui/SystemSettingsDialog.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/video/VideoWidget.cpp
    src/video/VideoWidget.h
)

target_link_libraries(CameraManagerApp
    PRIVATE
        Qt6::Widgets
        Qt6::Sql
        Qt6::Network
        Qt6::Multimedia
        Qt6::MultimediaWidgets
)

enable_testing()

qt_add_executable(CaptureRecordServiceTest
    tests/CaptureRecordServiceTest.cpp
    src/services/CaptureRecordService.cpp
    src/services/CaptureRecordService.h
    src/models/CaptureRecord.h
)

target_link_libraries(CaptureRecordServiceTest
    PRIVATE
        Qt6::Core
        Qt6::Sql
        Qt6::Test
)

target_compile_options(CaptureRecordServiceTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME CaptureRecordServiceTest COMMAND CaptureRecordServiceTest)

qt_add_executable(SystemSettingsServiceTest
    tests/SystemSettingsServiceTest.cpp
    src/models/SystemSettings.h
    src/services/SystemSettingsService.cpp
    src/services/SystemSettingsService.h
)

target_link_libraries(SystemSettingsServiceTest
    PRIVATE
        Qt6::Core
        Qt6::Test
)

target_compile_options(SystemSettingsServiceTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME SystemSettingsServiceTest COMMAND SystemSettingsServiceTest)

qt_add_executable(CaptureStorageServiceTest
    tests/CaptureStorageServiceTest.cpp
    src/services/CaptureStorageService.cpp
    src/services/CaptureStorageService.h
    src/models/CaptureRecord.h
    src/models/SystemSettings.h
)

target_link_libraries(CaptureStorageServiceTest
    PRIVATE
        Qt6::Core
        Qt6::Test
)

target_compile_options(CaptureStorageServiceTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME CaptureStorageServiceTest COMMAND CaptureStorageServiceTest)

qt_add_executable(MaintenanceControllerTest
    tests/MaintenanceControllerTest.cpp
    src/services/MaintenanceController.cpp
    src/services/MaintenanceController.h
    src/models/SystemSettings.h
)

target_link_libraries(MaintenanceControllerTest
    PRIVATE
        Qt6::Core
        Qt6::Test
)

target_compile_options(MaintenanceControllerTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME MaintenanceControllerTest COMMAND MaintenanceControllerTest)

qt_add_executable(DeviceManagerTest
    tests/DeviceManagerTest.cpp
    src/services/DeviceManager.cpp
    src/services/DeviceManager.h
    src/device/IDeviceClient.cpp
    src/device/IDeviceClient.h
    src/device/MockDeviceClient.cpp
    src/device/MockDeviceClient.h
    src/models/CaptureRecord.h
    src/models/Device.h
    src/models/DeviceConfig.h
    src/models/DeviceStatus.h
)

target_link_libraries(DeviceManagerTest
    PRIVATE
        Qt6::Core
        Qt6::Test
)

target_compile_options(DeviceManagerTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(DeviceManagerTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME DeviceManagerTest COMMAND DeviceManagerTest)

qt_add_executable(Rv1126bArchitectureTypesTest
    tests/Rv1126bArchitectureTypesTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/ports/IRtspPlayer.h
    src/rv1126b/ports/ISecretStore.h
    src/rv1126b/protocol/ApiCodec.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/DeviceDiscoveryService.h
    src/rv1126b/services/DeviceFleetService.h
    src/rv1126b/services/DeviceSession.h
    src/rv1126b/services/BoardFtpTaskSnapshotService.h
    src/rv1126b/services/EventSyncService.h
    src/rv1126b/services/EvidenceCache.h
    src/rv1126b/services/FtpService.h
)

target_link_libraries(Rv1126bArchitectureTypesTest
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Widgets
        Qt6::Test
)

target_compile_options(Rv1126bArchitectureTypesTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME Rv1126bArchitectureTypesTest COMMAND Rv1126bArchitectureTypesTest)

qt_add_executable(DeviceIntegrationControllerTest
    tests/DeviceIntegrationControllerTest.cpp
    src/rv1126b/application/DeviceIntegrationController.cpp
    src/rv1126b/application/DeviceIntegrationController.h
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/network/DirectDeviceProbeService.cpp
    src/rv1126b/network/DirectDeviceProbeService.h
    src/rv1126b/ports/IRtspPlayer.h
    src/rv1126b/ports/ISecretStore.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/DeviceDiscoveryService.h
    src/rv1126b/services/DeviceFleetService.h
)

target_link_libraries(DeviceIntegrationControllerTest
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Widgets
        Qt6::Test
)

target_compile_options(DeviceIntegrationControllerTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

qt_add_executable(ApiCodecTest
    tests/ApiCodecTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/protocol/ApiCodec.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/protocol/BoardApiCodec.cpp
    src/rv1126b/protocol/BoardApiCodec.h
)

target_link_libraries(ApiCodecTest
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
)

target_compile_options(ApiCodecTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME DeviceIntegrationControllerTest COMMAND DeviceIntegrationControllerTest)
set_tests_properties(DeviceIntegrationControllerTest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(DeviceIntegrationUiTest
    ${VIDEO_STREAMS_CONTROLLER_SOURCES}
    tests/DeviceIntegrationUiTest.cpp
    src/rv1126b/application/DeviceIntegrationController.cpp
    src/rv1126b/application/DeviceIntegrationController.h
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/network/DirectDeviceProbeService.cpp
    src/rv1126b/network/DirectDeviceProbeService.h
    src/rv1126b/ports/IRtspPlayer.h
    src/rv1126b/ports/ISecretStore.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/DeviceDiscoveryService.h
    src/rv1126b/services/DeviceFleetService.h
    src/ui/DeviceDiscoveryDialog.cpp
    src/ui/DeviceDiscoveryDialog.h
    src/ui/LivePreviewPanel.cpp
    src/ui/LivePreviewPanel.h
)

qt_add_executable(VideoStreamsTest
    tests/VideoStreamsTest.cpp
    ${VIDEO_STREAMS_CONTROLLER_SOURCES}
    src/rv1126b/ports/IRtspPlayer.h
    src/ui/LivePreviewPanel.cpp
    src/ui/LivePreviewPanel.h
)
target_link_libraries(VideoStreamsTest PRIVATE Qt6::Widgets Qt6::Network Qt6::Test)
target_compile_options(VideoStreamsTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME VideoStreamsTest COMMAND VideoStreamsTest)
set_tests_properties(VideoStreamsTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

target_link_libraries(DeviceIntegrationUiTest
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Widgets
        Qt6::Test
)

target_compile_options(DeviceIntegrationUiTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME DeviceIntegrationUiTest COMMAND DeviceIntegrationUiTest)
set_tests_properties(DeviceIntegrationUiTest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

target_compile_definitions(ApiCodecTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME ApiCodecTest COMMAND ApiCodecTest)

qt_add_executable(BoardApiClientTest
    tests/BoardApiClientTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/network/BoardApiClient.cpp
    src/rv1126b/network/BoardApiClient.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/ports/ISecretStore.h
    src/rv1126b/protocol/ApiCodec.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/protocol/BoardApiCodec.cpp
    src/rv1126b/protocol/BoardApiCodec.h
)

target_link_libraries(BoardApiClientTest
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
)

target_compile_options(BoardApiClientTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME BoardApiClientTest COMMAND BoardApiClientTest)

qt_add_executable(EventViewControllerTest
    tests/EventViewControllerTest.cpp
    src/rv1126b/application/EventViewController.cpp
    src/rv1126b/application/EventViewController.h
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/EventSyncService.h
    src/rv1126b/services/EvidenceCache.h
)

target_link_libraries(EventViewControllerTest
    PRIVATE Qt6::Core Qt6::Network Qt6::Test
)
target_compile_options(EventViewControllerTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME EventViewControllerTest COMMAND EventViewControllerTest)

qt_add_executable(EventViewUiTest
    tests/EventViewUiTest.cpp
    src/models/CaptureRecord.h
    src/models/Device.h
    src/models/DeviceConfig.h
    src/models/DeviceStatus.h
    src/models/table_models/CaptureRecordTableModel.cpp
    src/models/table_models/CaptureRecordTableModel.h
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/protocol/ApiDtos.h
    src/video/VideoWidget.cpp
    src/video/VideoWidget.h
)

target_link_libraries(EventViewUiTest
    PRIVATE Qt6::Core Qt6::Network Qt6::Widgets Qt6::Test
)
target_compile_options(EventViewUiTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME EventViewUiTest COMMAND EventViewUiTest)
set_tests_properties(EventViewUiTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(DeviceOperationsControllerTest
    tests/DeviceOperationsControllerTest.cpp
    src/rv1126b/application/DeviceOperationsController.cpp
    src/rv1126b/services/BoardFtpTaskSnapshotService.cpp
    src/rv1126b/application/DeviceOperationsController.h
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/FtpService.h
)
target_link_libraries(DeviceOperationsControllerTest
    PRIVATE Qt6::Core Qt6::Network Qt6::Test
)
target_compile_options(DeviceOperationsControllerTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME DeviceOperationsControllerTest COMMAND DeviceOperationsControllerTest)

qt_add_executable(DeviceOperationsUiTest
    tests/DeviceOperationsUiTest.cpp
    src/rv1126b/application/DeviceOperationsController.cpp
    src/rv1126b/services/BoardFtpTaskSnapshotService.cpp
    src/rv1126b/services/EmbeddedFtpReceiveServer.cpp
    src/rv1126b/services/EmbeddedFtpReceiveServer.h
    src/rv1126b/services/EmbeddedFtpReceiveWorker.h
    src/rv1126b/application/DeviceOperationsController.h
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/EventSyncService.h
    src/rv1126b/services/FtpService.h
    src/ui/Rv1126bDeviceManagementDialog.cpp
    src/ui/Rv1126bDeviceManagementDialog.h
)
target_link_libraries(DeviceOperationsUiTest
    PRIVATE Qt6::Core Qt6::Network Qt6::Widgets Qt6::Test
)
target_compile_options(DeviceOperationsUiTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME DeviceOperationsUiTest COMMAND DeviceOperationsUiTest)
set_tests_properties(DeviceOperationsUiTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(RtspPlayerTest
    tests/RtspPlayerTest.cpp
    ${RTSP_PLAYER_CORE_SOURCES}
    src/rv1126b/core/Result.h
)

target_link_libraries(RtspPlayerTest
    PRIVATE
        Qt6::Widgets
        Qt6::Test
)

target_compile_options(RtspPlayerTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME RtspPlayerTest COMMAND RtspPlayerTest)
set_tests_properties(RtspPlayerTest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_MEDIA_BACKEND=ffmpeg"
)

qt_add_executable(WindowsCredentialStoreTest
    tests/WindowsCredentialStoreTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/ports/ISecretStore.h
    src/rv1126b/security/WindowsCredentialStore.cpp
    src/rv1126b/security/WindowsCredentialStore.h
)

target_link_libraries(WindowsCredentialStoreTest
    PRIVATE
        Qt6::Core
        Qt6::Test
)

target_compile_options(WindowsCredentialStoreTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

if(BUILD_RTSP_SPIKE)
    qt_add_executable(Rv1126bRtspSpike
        tools/rtsp_spike/main.cpp
        tools/rtsp_spike/SpikeWindow.cpp
        tools/rtsp_spike/SpikeWindow.h
        ${RTSP_PLAYER_SOURCES}
        src/rv1126b/core/Result.h
    )

    target_link_libraries(Rv1126bRtspSpike
        PRIVATE
            Qt6::Widgets
            Qt6::Network
            Qt6::Multimedia
            Qt6::MultimediaWidgets
    )

    if(WIN32)
        target_link_libraries(Rv1126bRtspSpike PRIVATE psapi)
    endif()

    target_compile_options(Rv1126bRtspSpike
        PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
endif()

if(WIN32)
    target_link_libraries(WindowsCredentialStoreTest PRIVATE Advapi32)
endif()

add_test(NAME WindowsCredentialStoreTest COMMAND WindowsCredentialStoreTest)

qt_add_executable(SecurityBoundaryTest
    tests/SecurityBoundaryTest.cpp
)

target_link_libraries(SecurityBoundaryTest
    PRIVATE
        Qt6::Core
        Qt6::Test
)

target_compile_options(SecurityBoundaryTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(SecurityBoundaryTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME SecurityBoundaryTest COMMAND SecurityBoundaryTest)

qt_add_executable(DiscoveryAndSessionTest
    tests/DiscoveryAndSessionTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/network/UdpDeviceDiscoveryService.cpp
    src/rv1126b/network/UdpDeviceDiscoveryService.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/protocol/ApiCodec.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/protocol/BoardApiCodec.cpp
    src/rv1126b/protocol/BoardApiCodec.h
    src/rv1126b/services/BoardDeviceSession.cpp
    src/rv1126b/services/BoardDeviceSession.h
    src/rv1126b/services/DeviceDiscoveryService.h
    src/rv1126b/services/DeviceSession.h
)

target_link_libraries(DiscoveryAndSessionTest
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
)

target_compile_options(DiscoveryAndSessionTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME DiscoveryAndSessionTest COMMAND DiscoveryAndSessionTest)

qt_add_executable(BoardFtpServiceTest
    tests/BoardFtpServiceTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/BoardFtpService.cpp
    src/rv1126b/services/BoardFtpService.h
    src/rv1126b/services/FtpService.h
)

target_link_libraries(BoardFtpServiceTest
    PRIVATE
        Qt6::Core
        Qt6::Test
)

target_compile_options(BoardFtpServiceTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_test(NAME BoardFtpServiceTest COMMAND BoardFtpServiceTest)

qt_add_executable(EmbeddedFtpReceiveServerTest
    tests/EmbeddedFtpReceiveServerTest.cpp
    src/rv1126b/services/EmbeddedFtpReceiveServer.cpp
    src/rv1126b/services/EmbeddedFtpReceiveServer.h
    src/rv1126b/services/EmbeddedFtpReceiveWorker.h
)

target_link_libraries(EmbeddedFtpReceiveServerTest
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
)

target_compile_options(EmbeddedFtpReceiveServerTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME EmbeddedFtpReceiveServerTest COMMAND EmbeddedFtpReceiveServerTest)

qt_add_executable(SqliteEventRepositoryTest
    tests/SqliteEventRepositoryTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/storage/SqliteEventRepository.cpp
    src/rv1126b/storage/SqliteEventRepository.h
)

target_link_libraries(SqliteEventRepositoryTest
    PRIVATE
        Qt6::Core
        Qt6::Sql
        Qt6::Test
)

target_compile_options(SqliteEventRepositoryTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(SqliteEventRepositoryTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME SqliteEventRepositoryTest COMMAND SqliteEventRepositoryTest)

qt_add_executable(EventSyncServiceTest
    tests/EventSyncServiceTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/BoardEventSyncService.cpp
    src/rv1126b/services/BoardEventSyncService.h
    src/rv1126b/services/EventSyncService.h
    src/rv1126b/storage/SqliteEventRepository.cpp
    src/rv1126b/storage/SqliteEventRepository.h
)

target_link_libraries(EventSyncServiceTest
    PRIVATE
        Qt6::Core
        Qt6::Sql
        Qt6::Test
)

target_compile_options(EventSyncServiceTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(EventSyncServiceTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME EventSyncServiceTest COMMAND EventSyncServiceTest)

qt_add_executable(EvidenceCacheTest
    tests/EvidenceCacheTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/BoardEvidenceCache.cpp
    src/rv1126b/services/BoardEvidenceCache.h
    src/rv1126b/services/EvidenceCache.h
    src/rv1126b/storage/SqliteEventRepository.cpp
    src/rv1126b/storage/SqliteEventRepository.h
)

target_link_libraries(EvidenceCacheTest
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Sql
        Qt6::Test
)

target_compile_options(EvidenceCacheTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(EvidenceCacheTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME EvidenceCacheTest COMMAND EvidenceCacheTest)

qt_add_executable(LocalEvidenceServiceTest
    tests/LocalEvidenceServiceTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/EvidenceCache.h
    src/rv1126b/services/LocalEvidenceService.cpp
    src/rv1126b/services/LocalEvidenceService.h
    src/rv1126b/storage/SqliteEventRepository.cpp
    src/rv1126b/storage/SqliteEventRepository.h
)

target_link_libraries(LocalEvidenceServiceTest
    PRIVATE
        Qt6::Core
        Qt6::Sql
        Qt6::Test
)

target_compile_options(LocalEvidenceServiceTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(LocalEvidenceServiceTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME LocalEvidenceServiceTest COMMAND LocalEvidenceServiceTest)

qt_add_executable(FtpTaskSnapshotServiceTest
    tests/FtpTaskSnapshotServiceTest.cpp
    src/rv1126b/core/Result.h
    src/rv1126b/core/ValueTypes.h
    src/rv1126b/domain/Models.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/protocol/ApiDtos.h
    src/rv1126b/services/BoardFtpTaskSnapshotService.cpp
    src/rv1126b/services/BoardFtpTaskSnapshotService.h
    src/rv1126b/storage/SqliteEventRepository.cpp
    src/rv1126b/storage/SqliteEventRepository.h
)

target_link_libraries(FtpTaskSnapshotServiceTest
    PRIVATE
        Qt6::Core
        Qt6::Sql
        Qt6::Test
)

target_compile_options(FtpTaskSnapshotServiceTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(FtpTaskSnapshotServiceTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME FtpTaskSnapshotServiceTest COMMAND FtpTaskSnapshotServiceTest)

qt_add_executable(EvidenceCacheMaintenanceServiceTest
    tests/EvidenceCacheMaintenanceServiceTest.cpp
    src/models/SystemSettings.h
    src/rv1126b/services/EvidenceCacheMaintenanceService.cpp
    src/rv1126b/services/EvidenceCacheMaintenanceService.h
)

target_link_libraries(EvidenceCacheMaintenanceServiceTest
    PRIVATE
        Qt6::Core
        Qt6::Test
)

target_compile_options(EvidenceCacheMaintenanceServiceTest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(EvidenceCacheMaintenanceServiceTest
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME EvidenceCacheMaintenanceServiceTest COMMAND EvidenceCacheMaintenanceServiceTest)

qt_add_executable(ApplicationRuntimeTest
    tests/ApplicationRuntimeTest.cpp
    src/rv1126b/application/Rv1126bApplicationRuntime.h
    src/rv1126b/application/Rv1126bApplicationRuntime.cpp
    src/rv1126b/ports/IBoardApiClient.h
    src/rv1126b/ports/IEventRepository.h
    src/rv1126b/ports/ISecretStore.h
    src/rv1126b/ports/IRtspPlayer.h
    src/rv1126b/network/BoardApiClient.cpp
    src/rv1126b/network/DirectDeviceProbeService.cpp
    src/rv1126b/network/UdpDeviceDiscoveryService.cpp
    src/rv1126b/protocol/BoardApiCodec.cpp
    src/rv1126b/services/BoardDeviceFleetService.cpp
    src/rv1126b/services/BoardDeviceSession.cpp
    src/rv1126b/services/BoardEventSyncService.cpp
    src/rv1126b/services/BoardEvidenceCache.cpp
    src/rv1126b/services/BoardFtpService.cpp
    src/rv1126b/services/BoardFtpTaskSnapshotService.cpp
    src/rv1126b/services/EmbeddedFtpReceiveServer.cpp
    src/rv1126b/services/EmbeddedFtpReceiveServer.h
    src/rv1126b/services/EmbeddedFtpReceiveWorker.h
    src/rv1126b/services/EvidenceCacheMaintenanceService.cpp
    src/rv1126b/services/DeviceDiscoveryService.h
    src/rv1126b/services/DeviceFleetService.h
    src/rv1126b/services/DeviceSession.h
    src/rv1126b/services/EventSyncService.h
    src/rv1126b/services/EvidenceCache.h
    src/rv1126b/services/FtpService.h
    src/rv1126b/security/WindowsCredentialStore.cpp
    src/rv1126b/storage/SqliteEventRepository.cpp
    src/rv1126b/infrastructure/video/QtMultimediaRtspPlayer.cpp
    src/rv1126b/infrastructure/video/QtMultimediaRtspPlayer.h
    src/rv1126b/infrastructure/video/IMediaPlaybackBackend.h
    src/rv1126b/infrastructure/video/QtMediaPlaybackBackend.cpp
    src/rv1126b/infrastructure/video/QtMultimediaRtspPlayerFactory.cpp
)
target_link_libraries(ApplicationRuntimeTest PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Test)
target_compile_options(ApplicationRuntimeTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME ApplicationRuntimeTest COMMAND ApplicationRuntimeTest)
set_tests_properties(ApplicationRuntimeTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(DirectDeviceProbeServiceTest
    tests/DirectDeviceProbeServiceTest.cpp
    src/rv1126b/network/DirectDeviceProbeService.cpp
    src/rv1126b/network/DirectDeviceProbeService.h
    src/rv1126b/protocol/BoardApiCodec.cpp
    src/rv1126b/protocol/BoardApiCodec.h
)
target_link_libraries(DirectDeviceProbeServiceTest PRIVATE Qt6::Network Qt6::Test)
target_compile_options(DirectDeviceProbeServiceTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME DirectDeviceProbeServiceTest COMMAND DirectDeviceProbeServiceTest)

target_compile_options(CameraManagerApp
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(CameraManagerApp
    PRIVATE
        CAMERA_MANAGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

if(WIN32)
    target_link_libraries(CameraManagerApp PRIVATE Advapi32)
    set_target_properties(CameraManagerApp PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()
