#include "ArchiveJob.hpp"

#include <QMetaObject>

#include <chrono>
#include <memory>
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

    struct Outcome {
        bool cancelled = false;
        bool failed = false;
        QString message;
    };
    const auto outcome = std::make_shared<Outcome>();
    auto* thread = QThread::create([this, outcome, task = std::move(task)]() {
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
        } catch (const zipdesk::ArchiveError& error) {
            if (error.kind() == zipdesk::ErrorKind::Cancelled || m_cancel.load()) {
                outcome->cancelled = true;
            } else {
                outcome->failed = true;
                outcome->message = QString::fromStdWString(error.message());
            }
        } catch (const std::exception& error) {
            outcome->failed = true;
            outcome->message = QString::fromLocal8Bit(error.what());
        }
    });

    m_thread = thread;
    connect(thread, &QThread::finished, this, [this, thread, outcome]() {
        if (m_thread == thread) {
            m_thread = nullptr;
        }
        thread->deleteLater();
        m_running.store(false);
        // Completion callbacks may immediately start another job (for example reload).
        if (outcome->cancelled) emit cancelled();
        else if (outcome->failed) emit failed(outcome->message);
        else emit succeeded();
    });
    thread->start();
}
