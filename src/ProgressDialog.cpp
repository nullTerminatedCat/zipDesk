#include "ProgressDialog.hpp"

#include <algorithm>

#include <QCloseEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

ProgressDialog::ProgressDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("正在处理");
    setModal(false);
    setMinimumWidth(440);
    setStyleSheet(
        "QDialog { background: #f3efe6; }"
        "QLabel { color: #1f2430; }"
        "QProgressBar { border: none; background: #efe6da; border-radius: 5px; min-height: 8px; }"
        "QProgressBar::chunk { background: #0f6e6b; border-radius: 5px; }"
        "QPushButton { background: #fffdf8; border: 1px solid #e4d8c8; border-radius: 10px; padding: 8px 16px; }");
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    m_file = new QLabel("准备中…", this);
    m_percent = new QLabel("0%", this);
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 0);
    m_bar->setTextVisible(false);
    m_cancel = new QPushButton("取消", this);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_file);
    layout->addWidget(m_percent);
    layout->addWidget(m_bar);
    layout->addWidget(m_cancel, 0, Qt::AlignRight);
    connect(m_cancel, &QPushButton::clicked, this, &ProgressDialog::requestCancel);
}

void ProgressDialog::reset(const QString& title) {
    setWindowTitle(title);
    m_file->setText("准备中…");
    m_percent->setText("0%");
    m_bar->setRange(0, 0);
    m_bar->setValue(0);
    m_total = 0;
    m_cancel->setEnabled(true);
    m_cancel->setText("取消");
}

void ProgressDialog::setTotal(quint64 total) {
    m_total = total;
    if (total == 0) {
        m_bar->setRange(0, 0);
        return;
    }
    m_bar->setRange(0, 1000);
    m_bar->setValue(0);
    m_percent->setText("0%");
}

void ProgressDialog::setProcessed(quint64 processed) {
    if (m_total == 0) {
        return;
    }
    const int value = static_cast<int>((processed * 1000) / m_total);
    m_bar->setValue(std::min(value, 1000));
    m_percent->setText(QString::number(value / 10) + "%");
}

void ProgressDialog::setCurrentFile(const QString& file) {
    const QFontMetrics metrics(m_file->font());
    m_file->setText(metrics.elidedText(file, Qt::ElideMiddle, 400));
    m_file->setToolTip(file);
}

void ProgressDialog::closeEvent(QCloseEvent* event) {
    requestCancel();
    event->ignore();
}

void ProgressDialog::requestCancel() {
    m_cancel->setEnabled(false);
    m_cancel->setText("正在取消…");
    emit cancelRequested();
}
