#pragma once

#include <QDialog>
#include <QElapsedTimer>

class QLabel;
class QProgressBar;
class QPushButton;

class ProgressDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProgressDialog(QWidget* parent = nullptr);

    void reset(const QString& title);

public slots:
    void setTotal(quint64 total);
    void setProcessed(quint64 processed);
    void setCurrentFile(const QString& file);

signals:
    void cancelRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void requestCancel();
    void updateMetrics();

    QLabel* m_metrics = nullptr;
    QElapsedTimer m_elapsed;
    quint64 m_processed = 0;
    qint64 m_lastProgressAt = 0;
    QLabel* m_file = nullptr;
    QLabel* m_percent = nullptr;
    QProgressBar* m_bar = nullptr;
    QPushButton* m_cancel = nullptr;
    quint64 m_total = 0;
    bool m_cancelRequested = false;
};
