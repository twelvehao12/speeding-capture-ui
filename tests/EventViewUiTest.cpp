#include "../src/models/table_models/CaptureRecordTableModel.h"
#include "../src/video/VideoWidget.h"
#include "../src/services/UiPerformanceMonitor.h"

#include <QImage>
#include <QTemporaryDir>
#include <QtTest>
#include <QPersistentModelIndex>

using namespace rv1126b;

class EventViewUiTest final : public QObject
{
    Q_OBJECT
private slots:
    void compositeIdentityUpsertsWithoutDuplicate();
    void terminalPlateFiltersAndTimeWarningAreVisible();
    void evidencePreviewLoadsJpegAndShowsOfflineOrRetryState();
    void rapidImageSelectionKeepsLastImageAndRejectsOldState();
    void modelPrunesEvidenceAndKeepsPersistentSelection();
    void performanceLogWritesBoundedSamples();
};

static VehicleEvent uiEvent(qint64 eventId, qint64 trackId, OcrStatus status)
{
    VehicleEvent event;
    event.identity = {QStringLiteral("dev-a"), eventId, trackId};
    event.eventTime.epochMs = 1000;
    event.eventTime.sourceEpochMs = 1000;
    event.eventTime.quality.value = TimeQuality::NativeUtc;
    event.ocrStatus.value = status;
    return event;
}

void EventViewUiTest::compositeIdentityUpsertsWithoutDuplicate()
{
    CaptureRecordTableModel model;
    VehicleEvent queued = uiEvent(1, 10, OcrStatus::Queued);
    model.setVehicleEvents({queued});
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), CaptureRecordTableModel::PlateStateRole).toString(),
             QStringLiteral("pending"));

    queued.ocrStatus.value = OcrStatus::Matched;
    queued.plateText = QStringLiteral("粤B12345");
    model.upsertVehicleEvent(queued);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.vehicleEventAt(0)->plateText, QStringLiteral("粤B12345"));

    model.upsertVehicleEvent(uiEvent(1, 11, OcrStatus::NoPlate));
    QCOMPARE(model.rowCount(), 2);
}

void EventViewUiTest::terminalPlateFiltersAndTimeWarningAreVisible()
{
    CaptureRecordTableModel model;
    VehicleEvent event = uiEvent(1, 1, OcrStatus::NoPlate);
    event.eventTime.quality.value = TimeQuality::BoardEpochUnverified;
    model.setVehicleEvents({event});
    QCOMPARE(model.data(model.index(0, 0), CaptureRecordTableModel::PlateStateRole).toString(),
             QStringLiteral("unknown"));
    QVERIFY(model.data(model.index(0, CaptureRecordTableModel::TimeQualityColumn)).toString()
                .contains(QStringLiteral("未校验")));
    QVERIFY(model.data(model.index(0, 0), Qt::ToolTipRole).toString()
                .contains(QStringLiteral("不可作为可靠 UTC")));
}

void EventViewUiTest::evidencePreviewLoadsJpegAndShowsOfflineOrRetryState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("evidence.jpg"));
    QImage image(40, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(path, "JPEG"));

    VideoWidget widget(VideoWidget::Mode::Snapshot);
    const VehicleEvent event = uiEvent(1, 1, OcrStatus::Matched);
    widget.setVehicleEvent(&event);
    EvidenceCacheEntry available;
    available.identity = event.identity;
    available.status = EvidenceCacheStatus::Available;
    available.localFilePath = path;
    widget.setEvidenceState(available, false);
    QTRY_VERIFY(widget.hasDecodedEvidence());
    QVERIFY(widget.evidenceMessage().isEmpty());

    EvidenceCacheEntry missing;
    missing.identity = event.identity;
    missing.status = EvidenceCacheStatus::Missing;
    missing.failureCode = QStringLiteral("offline_not_cached");
    widget.setEvidenceState(missing, false);
    QVERIFY(widget.evidenceMessage().contains(QStringLiteral("设备离线")));

    EvidenceCacheEntry retry;
    retry.identity = event.identity;
    retry.status = EvidenceCacheStatus::RetryWait;
    widget.setEvidenceState(retry, true);
    QVERIFY(widget.evidenceMessage().contains(QStringLiteral("等待重试")));
}

void EventViewUiTest::rapidImageSelectionKeepsLastImageAndRejectsOldState()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("last.jpg"));
    QImage image(2560, 1440, QImage::Format_RGB32);
    image.fill(Qt::green);
    QVERIFY(image.save(path, "JPEG"));
    VideoWidget widget(VideoWidget::Mode::Snapshot);
    EvidenceCacheEntry old;
    for (int i = 0; i < 1000; ++i) {
        const auto event = uiEvent(i, 1, OcrStatus::Matched);
        widget.setVehicleEvent(&event);
        EvidenceCacheEntry entry;
        entry.identity = event.identity;
        entry.status = EvidenceCacheStatus::Available;
        entry.localFilePath = i == 999 ? path : dir.filePath(QStringLiteral("missing.jpg"));
        widget.setEvidenceState(entry, true);
        if (i == 998) old = entry;
    }
    widget.setEvidenceState(old, true);
    QTRY_VERIFY(widget.hasDecodedEvidence());
    QVERIFY(widget.evidenceMessage().isEmpty());
    widget.clearVehicleEvent();
    widget.setEvidenceState(old, true);
    QTest::qWait(150);
    QVERIFY(!widget.hasDecodedEvidence());
}

void EventViewUiTest::modelPrunesEvidenceAndKeepsPersistentSelection()
{
    CaptureRecordTableModel model;
    auto selected = uiEvent(1, 1, OcrStatus::Matched);
    model.setVehicleEvents({selected});
    QPersistentModelIndex selection(model.index(0, 0));
    for (int i = 2; i < 1000; ++i) {
        auto event = uiEvent(i, 1, OcrStatus::Matched);
        model.upsertVehicleEvent(event, 10);
        EvidenceCacheEntry entry;
        entry.identity = event.identity;
        entry.status = EvidenceCacheStatus::Queued;
        model.setEvidenceState(entry);
        if (i == 2) {
            QVERIFY(selection.isValid());
            QCOMPARE(model.vehicleEventAt(selection.row())->identity, selected.identity);
        }
        QVERIFY(model.property("evidenceStateCount").toInt() <= 10);
    }
    QVERIFY(!selection.isValid());
    QCOMPARE(model.rowCount(), 10);
    model.setVehicleEvents({selected});
    QCOMPARE(model.property("evidenceStateCount").toInt(), 0);
}

void EventViewUiTest::performanceLogWritesBoundedSamples()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("performance.csv"));
    const auto previous = qgetenv("CAMERA_PERF_LOG");
    qputenv("CAMERA_PERF_LOG", path.toUtf8());
    auto* monitor = new UiPerformanceMonitor(this);
    if (previous.isNull()) qunsetenv("CAMERA_PERF_LOG");
    else qputenv("CAMERA_PERF_LOG", previous);
    QTest::qWait(5250);
    delete monitor; // Joins the writer before inspecting the file.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto lines = file.readAll().trimmed().split('\n');
    QVERIFY(lines.size() >= 2);
    QVERIFY(lines.first().contains("heartbeat_p95_ms"));
    QCOMPARE(lines.last().split(',').size(), 12);
}

QTEST_MAIN(EventViewUiTest)
#include "EventViewUiTest.moc"

