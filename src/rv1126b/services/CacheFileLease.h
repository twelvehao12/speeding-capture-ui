#pragma once

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <memory>

namespace rv1126b {
// Coordinates readers/download promotion with cache cleanup without holding a
// mutex during disk I/O. An exclusive lease cannot overlap any reader.
class CacheFileLease final
{
public:
    static std::shared_ptr<CacheFileLease> acquire(const QString& path, bool exclusive = false)
    {
        QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
        key = key.toCaseFolded();
#endif
        auto& state = registry();
        QMutexLocker lock(&state.mutex);
        if (state.exclusive.contains(key) || (exclusive && state.readers.value(key) > 0)) return {};
        if (exclusive) state.exclusive.insert(key);
        else ++state.readers[key];
        return std::shared_ptr<CacheFileLease>(new CacheFileLease(key, exclusive));
    }
    ~CacheFileLease()
    {
        auto& state = registry();
        QMutexLocker lock(&state.mutex);
        if (exclusive_) state.exclusive.remove(key_);
        else if (--state.readers[key_] == 0) state.readers.remove(key_);
    }
private:
    struct Registry { QMutex mutex; QHash<QString, int> readers; QSet<QString> exclusive; };
    // Worker pools can finish after other function-local statics are torn
    // down. Keep this single, bounded registry alive for process lifetime.
    static Registry& registry() { static auto* state = new Registry; return *state; }
    CacheFileLease(QString key, bool exclusive) : key_(std::move(key)), exclusive_(exclusive) {}
    QString key_;
    bool exclusive_;
};
} // namespace rv1126b
