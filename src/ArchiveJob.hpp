#pragma once

#include "ArchiveService.hpp"

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <functional>
#include <optional>

class ArchiveJob : public QObject {
    Q_OBJECT

public:
    explicit ArchiveJob(QObject* parent = nullptr);

    bool isRunning() const;
    void requestCancel();
    void setPasswordPrompt(std::function<std::optional<QString>(const QString& prompt)> prompt);
    void start(std::function<void(const zipdesk::OperationHooks& hooks)> task);

signals:
    void totalChanged(quint64 total);
    void progressChanged(quint64 processed);
    void fileChanged(const QString& file);
    void succeeded();
    void failed(const QString& message);
    void cancelled();

private:
    std::function<std::optional<QString>(const QString&)> m_prompt;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    QThread* m_thread = nullptr;
};
