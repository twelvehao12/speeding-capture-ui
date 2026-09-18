#pragma once

#include "../ports/IBoardApiClient.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>

namespace rv1126b {

// Network buffering and disk writes live together on the I/O thread. Only the
// small completion value crosses back to the API client's thread.
class FileDownloadExecutor final : public QObject
{
public:
    struct PendingFile {
        QString path;
        bool delivered = false;
        ~PendingFile() { if (!delivered && !path.isEmpty()) QFile::remove(path); }
    };
    using Completion = std::function<void(int, QByteArray, ApiResult<EvidenceDownloadResult>)>;

    explicit FileDownloadExecutor(QObject* parent = nullptr) : QObject(parent)
    {
        worker_ = new Worker;
        worker_->moveToThread(&thread_);
        thread_.setObjectName(QStringLiteral("evidence-download-io"));
        connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
        thread_.start();
    }

    ~FileDownloadExecutor() override
    {
        thread_.quit();
        thread_.wait();
    }

    void start(const RequestId& id, QNetworkRequest request, QString path,
               QString contentTypePrefix, int firstResponseTimeoutMs, Completion completion)
    {
        QMetaObject::invokeMethod(worker_, [this, id, request, path, contentTypePrefix,
                                           firstResponseTimeoutMs, completion = std::move(completion)]() mutable {
            worker_->start(id, request, path, contentTypePrefix, firstResponseTimeoutMs,
                [this, completion = std::move(completion)](int status, QByteArray payload,
                                                           ApiResult<EvidenceDownloadResult> result) mutable {
                    auto file = std::make_shared<PendingFile>();
                    if (result) file->path = result.value().partFilePath;
                    QMetaObject::invokeMethod(this, [completion = std::move(completion), status,
                                                    file, payload = std::move(payload), result = std::move(result)]() mutable {
                        file->delivered = true;
                        completion(status, std::move(payload), std::move(result));
                    }, Qt::QueuedConnection);
                });
        }, Qt::QueuedConnection);
    }

    void cancel(const RequestId& id)
    {
        QMetaObject::invokeMethod(worker_, [worker = worker_, id] { worker->cancel(id); }, Qt::QueuedConnection);
    }

private:
    static ApiError error(QString code, QString message, ApiErrorCategory category)
    {
        ApiError e;
        e.code = std::move(code);
        e.message = std::move(message);
        e.category = category;
        e.retryable = category == ApiErrorCategory::Network;
        return e;
    }

    class Job final : public QObject
    {
    public:
        Job(QObject* parent, QString path) : QObject(parent), file_(std::move(path)) {}
        ~Job() override
        {
            file_.close();
            if (!retained_) QFile::remove(file_.fileName());
        }

        void read()
        {
            if (!reply_) return;
            const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            while (reply_->bytesAvailable() > 0) {
                const QByteArray block = reply_->read(64 * 1024);
                if (block.isEmpty()) break;
                if (status < 200 || status >= 300) {
                    errorBody_.append(block.left(qMax(0, 64 * 1024 - int(errorBody_.size()))));
                    continue;
                }
                if (writeFailed_) continue;
                if (!file_.isOpen()) {
                    const QFileInfo info(file_.fileName());
                    if (!QDir().mkpath(info.absolutePath()) || !file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        writeFailed_ = true;
                        continue;
                    }
                }
                if (file_.write(block) != block.size()) writeFailed_ = true;
                received_ += block.size();
            }
        }

        QPointer<QNetworkReply> reply_;
        QFile file_;
        QByteArray errorBody_;
        qint64 received_ = 0;
        bool writeFailed_ = false;
        bool retained_ = false;
        bool cancelled_ = false;
        bool timedOut_ = false;
    };

    class Worker final : public QObject
    {
    public:
        ~Worker() override
        {
            const auto jobs = jobs_.values();
            jobs_.clear();
            for (auto* job : jobs) {
                if (job->reply_) {
                    job->reply_->disconnect();
                    job->reply_->abort();
                    delete job->reply_.data();
                }
                delete job;
            }
        }
        void start(const RequestId& id, const QNetworkRequest& request, const QString& path,
                   const QString& prefix, int timeout, Completion completion)
        {
            if (!network_) network_ = new QNetworkAccessManager(this);
            auto* job = new Job(this, path);
            jobs_.insert(id, job);
            auto* reply = network_->get(request);
            job->reply_ = reply;
            reply->setReadBufferSize(256 * 1024);
            auto* timer = new QTimer(job);
            timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, job, [job] {
                job->timedOut_ = true;
                if (job->reply_) job->reply_->abort();
            });
            connect(reply, &QNetworkReply::metaDataChanged, timer, &QTimer::stop);
            connect(reply, &QNetworkReply::readyRead, job, [job] { job->read(); });
            connect(reply, &QNetworkReply::finished, job,
                [this, id, job, reply, timer, prefix, completion = std::move(completion)]() mutable {
                    timer->stop();
                    job->read();
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    ApiError failure;
                    if (job->cancelled_) failure = error("cancelled", "Request cancelled.", ApiErrorCategory::Cancelled);
                    else if (job->timedOut_) failure = error("network_connect_timeout", "Board connection timed out.", ApiErrorCategory::Network);
                    else if (reply->error() != QNetworkReply::NoError)
                        failure = error("network_error", "Board network request failed.", ApiErrorCategory::Network);
                    else if (status < 200 || status >= 300)
                        failure = error("http_error", "Board download failed.", ApiErrorCategory::Protocol);
                    else if (job->writeFailed_ || !job->file_.isOpen() || !job->file_.flush())
                        failure = error("evidence_write_failed", "Could not write evidence part file.", ApiErrorCategory::Storage);
                    const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
                    if (failure.code.isEmpty() && !prefix.isEmpty() && !contentType.startsWith(prefix))
                        failure = error("invalid_content_type", "Unexpected download content type.", ApiErrorCategory::Protocol);
                    bool lengthOk = false;
                    const qint64 expected = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&lengthOk);
                    if (failure.code.isEmpty() && (!lengthOk || expected < 0 || expected != job->received_))
                        failure = error("invalid_content_length", "Evidence length does not match Content-Length.", ApiErrorCategory::Protocol);
                    job->file_.close();
                    jobs_.remove(id);
                    if (failure.code.isEmpty()) {
                        job->retained_ = true;
                        EvidenceDownloadResult result;
                        result.partFilePath = job->file_.fileName();
                        result.contentType = contentType;
                        result.expectedContentLength = expected;
                        result.receivedBytes = job->received_;
                        completion(status, {}, ApiResult<EvidenceDownloadResult>::success(result));
                    } else {
                        QFile::remove(job->file_.fileName());
                        completion(status, job->errorBody_, ApiResult<EvidenceDownloadResult>::failure(failure));
                    }
                    reply->deleteLater();
                    job->deleteLater();
                });
            timer->start(timeout);
        }

        void cancel(const RequestId& id)
        {
            if (auto* job = jobs_.value(id)) {
                job->cancelled_ = true;
                if (job->reply_) job->reply_->abort();
            }
        }
        QHash<RequestId, Job*> jobs_;
        QNetworkAccessManager* network_ = nullptr;
    };
    QThread thread_;
    Worker* worker_ = nullptr;
};
} // namespace rv1126b
