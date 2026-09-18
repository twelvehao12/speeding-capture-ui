#pragma once

#include "../CaptureRecord.h"
#include "../../rv1126b/domain/Models.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QVector>

class CaptureRecordTableModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        TimeColumn,
        PlateColumn,
        PlateColorColumn,
        EventTypeColumn,
        DeviceIdColumn,
        DirectionColumn,
        CoordinateColumn,
        RemarkColumn,
        SpeedColumn,
        TimeQualityColumn,
        EvidenceStatusColumn,
        ColumnCount
    };

    enum Role {
        RecordIdRole = Qt::UserRole + 1,
        PlateStateRole,
        EventIdentityRole,
        VehicleEventRole,
        RealEventRole
    };

    explicit CaptureRecordTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setRecords(const QVector<CaptureRecord>& records);
    void setVehicleEvents(const QVector<rv1126b::VehicleEvent>& events);
    void upsertVehicleEvent(const rv1126b::VehicleEvent& event, int maximumRows = 100);
    void upsertVehicleEvents(const QVector<rv1126b::VehicleEvent>& events, int maximumRows);
    void removeVehicleEvent(const rv1126b::EventIdentity& identity);
    void setEvidenceState(const rv1126b::EvidenceCacheEntry& entry);
    void addRecord(const CaptureRecord& record);
    bool removeRecord(int row);
    bool removeRecordById(const QString& id);
    void clear();
    const CaptureRecord* recordAt(int row) const;
    const rv1126b::VehicleEvent* vehicleEventAt(int row) const;
    std::optional<rv1126b::EvidenceCacheEntry> evidenceStateAt(int row) const;
    const CaptureRecord* latestForDevice(const QString& deviceId) const;
    int recordCount() const;
    bool realEventMode() const;

private:
    QString plateStateFor(const rv1126b::VehicleEvent& event) const;
    void rebuildEventIndex();

    QVector<CaptureRecord> records_;
    QVector<rv1126b::VehicleEvent> events_;
    QHash<rv1126b::EventIdentity, rv1126b::EvidenceCacheEntry> evidenceByIdentity_;
    bool realEventMode_ = false;
    QHash<rv1126b::EventIdentity, int> rowByIdentity_;
};
