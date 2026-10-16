#pragma once

#include <QDialog>

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

    QLabel* m_file = nullptr;
    QLabel* m_percent = nullptr;
    QProgressBar* m_bar = nullptr;
    QPushButton* m_cancel = nullptr;
    quint64 m_total = 0;
    bool m_cancelRequested = false;
};
