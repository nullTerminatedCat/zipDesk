#include "ArchiveJob.hpp"

#include <QMetaObject>

#include <chrono>
#include <utility>

ArchiveJob::ArchiveJob(QObject* parent)
    : QObject(parent) {}

bool ArchiveJob::isRunning() const {
    return m_running.load();
}

void ArchiveJob::requestCancel() {
    m_cancel.store(true);
}

void ArchiveJob::setPasswordPrompt(std::function<std::optional<QString>(const QString& prompt)> prompt) {
    m_prompt = std::move(prompt);
}

void ArchiveJob::start(std::function<void(const zipdesk::OperationHooks& hooks)> task) {
    if (m_running.load()) {
        return;
    }
    m_cancel.store(false);
    m_running.store(true);

    auto* thread = QThread::create([this, task = std::move(task)]() {
        zipdesk::OperationHooks hooks;
        hooks.cancel = &m_cancel;
        auto lastEmit = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        hooks.onTotal = [this](std::uint64_t total) { emit totalChanged(total); };
        hooks.onProgress = [this, lastEmit](std::uint64_t processed) mutable {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastEmit < std::chrono::milliseconds(50)) {
                return;
            }
            lastEmit = now;
            emit progressChanged(processed);
        };
        hooks.onFile = [this](const std::wstring& file) { emit fileChanged(QString::fromStdWString(file)); };
        hooks.askPassword = [this](const std::wstring& prompt) -> std::optional<std::wstring> {
            if (!m_prompt) {
                return std::nullopt;
            }
            std::optional<QString> answer;
            QMetaObject::invokeMethod(
                this,
                [&]() { answer = m_prompt(QString::fromStdWString(prompt)); },
                Qt::BlockingQueuedConnection);
            if (!answer) {
                return std::nullopt;
            }
            return answer->toStdWString();
        };

        try {
            task(hooks);
            emit succeeded();
        } catch (const zipdesk::ArchiveError& error) {
            if (error.kind() == zipdesk::ErrorKind::Cancelled || m_cancel.load()) {
                emit cancelled();
            } else {
                emit failed(QString::fromStdWString(error.message()));
            }
        } catch (const std::exception& error) {
            emit failed(QString::fromLocal8Bit(error.what()));
        }
        m_running.store(false);
    });

    m_thread = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_thread == thread) {
            m_thread = nullptr;
        }
        thread->deleteLater();
    });
    thread->start();
}
