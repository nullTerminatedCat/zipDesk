#include "ConflictDialog.hpp"

#include <QButtonGroup>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
using zipdesk::OverwritePolicy;

QString sizeText(qint64 bytes) {
    if (bytes < 1024) return QString("%1 字节").arg(bytes);
    const double kb = bytes / 1024.0;
    if (kb < 1024) return QString("%1 KB").arg(kb, 0, 'f', 1);
    return QString("%1 MB").arg(kb / 1024.0, 0, 'f', 1);
}

struct Option {
    OverwritePolicy policy;
    const char* text;
    const char* icon;
    const char* name;
};
const Option kOptions[] = {
    {OverwritePolicy::KeepBoth, "保留两者", ":/assets/icons/conflict-copy.png", "keepBoth"},
    {OverwritePolicy::Skip, "跳过", ":/assets/icons/conflict-skip.png", "skip"},
    {OverwritePolicy::Overwrite, "覆盖", ":/assets/icons/conflict-refresh.png", "overwrite"},
};
}  // namespace

ConflictDialog::ConflictDialog(const QVector<Item>& items, const QString& outputDir, QWidget* parent)
    : QDialog(parent), m_items(items) {
    setWindowTitle("逐项选择同名文件");
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumSize(600, 420);
    resize(680, 520);
    setStyleSheet(
        "QWidget:focus{outline: none;}"
        "QDialog { background: #f5f7fa; color: #20332f; }"
        "QLabel { color: #20332f; }"
        "QLabel#conflictTitle { font-size: 15px; font-weight: 600; }"
        "QLabel#conflictHint, QLabel#conflictMeta, QLabel#conflictSummary { color: #667b75; font-size: 12px; }"
        "QLabel#conflictName { font-weight: 600; }"
        "QFrame#conflictRow { background: #ffffff; border: 1px solid #dfe7e4; border-radius: 8px; }"
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #c9d6d2; border-radius: 4px; min-height: 32px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QWidget#conflictList { background: transparent; }"
        "QToolButton { background: #ffffff; color: #20332f; border: 1px solid #dfe7e4; padding: 5px 10px; }"
        "QToolButton#keepBoth { border-top-left-radius: 7px; border-bottom-left-radius: 7px; }"
        "QToolButton#overwrite { border-top-right-radius: 7px; border-bottom-right-radius: 7px; }"
        "QToolButton:hover { background: #edf6f2; }"
        "QToolButton:checked { background: #dcefe7; border-color: #147d70; color: #0f5f55; font-weight: 600; }"
        "QToolButton:focus { border: 2px solid #147d70; padding: 4px 9px; }"
        "QPushButton { background: #ffffff; color: #20332f; border: 1px solid #dfe7e4; border-radius: 8px; padding: 7px 14px; }"
        "QPushButton:hover { background: #edf6f2; border-color: #8cbbae; }"
        "QPushButton:focus { background: #edf6f2; border: 2px solid #147d70; padding: 6px 13px; }"
        "QPushButton#confirmConflicts { background: #147d70; color: #ffffff; border-color: #147d70; }"
        "QPushButton#confirmConflicts:hover { background: #10685d; }");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(12);

    auto* header = new QHBoxLayout;
    auto* warn = new QLabel(this);
    warn->setPixmap(QIcon(":/assets/icons/conflict-warning.png").pixmap(28, 28));
    warn->setAlignment(Qt::AlignTop);
    auto* headerText = new QVBoxLayout;
    auto* title = new QLabel(QString("%1 个文件在目标位置已存在").arg(items.size()), this);
    title->setObjectName("conflictTitle");
    auto* hint = new QLabel(QString("为每个文件选择处理方式。默认“保留两者”，不会改动原文件。\n目标目录：%1")
                                .arg(QDir::toNativeSeparators(outputDir)), this);
    hint->setObjectName("conflictHint");
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    headerText->addWidget(title);
    headerText->addWidget(hint);
    header->addWidget(warn);
    header->addSpacing(8);
    header->addLayout(headerText, 1);
    layout->addLayout(header);

    auto* bulk = new QHBoxLayout;
    auto* bulkLabel = new QLabel("全部应用：", this);
    bulk->addWidget(bulkLabel);
    for (const Option& option : kOptions) {
        auto* button = new QPushButton(QIcon(option.icon), QString("全部") + option.text, this);
        button->setObjectName(QString("all_") + option.name);
        button->setIconSize(QSize(16, 16));
        button->setAutoDefault(false);
        const OverwritePolicy policy = option.policy;
        connect(button, &QPushButton::clicked, this, [this, policy]() { setAll(policy); });
        bulk->addWidget(button);
    }
    bulk->addStretch(1);
    layout->addLayout(bulk);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* list = new QWidget(scroll);
    list->setObjectName("conflictList");
    auto* rows = new QVBoxLayout(list);
    rows->setContentsMargins(0, 0, 10, 0);
    rows->setSpacing(8);
    const QIcon fileIcon(":/assets/icons/conflict-file-common.png");
    for (int row = 0; row < items.size(); ++row) {
        const Item& item = items[row];
        auto* frame = new QFrame(list);
        frame->setObjectName("conflictRow");
        auto* line = new QHBoxLayout(frame);
        line->setContentsMargins(12, 10, 12, 10);
        line->setSpacing(10);
        auto* icon = new QLabel(frame);
        icon->setPixmap(fileIcon.pixmap(22, 22));
        line->addWidget(icon);
        auto* text = new QVBoxLayout;
        text->setSpacing(2);
        const QString path = QDir::toNativeSeparators(QString::fromStdWString(item.path));
        auto* name = new QLabel(QFileInfo(QString::fromStdWString(item.path)).fileName(), frame);
        name->setObjectName("conflictName");
        name->setTextFormat(Qt::PlainText);
        name->setToolTip(path);
        const QString folder = QFileInfo(path).path();
        const QString where = folder == "." ? QString() : QString("位于 %1 · ").arg(folder);
        auto* meta = new QLabel(QString("%1压缩包内 %2 · 现有 %3")
                                    .arg(where, sizeText(static_cast<qint64>(item.archiveSize)),
                                         item.targetIsDir ? QString("文件夹") : sizeText(item.targetSize)),
                                frame);
        meta->setObjectName("conflictMeta");
        meta->setTextFormat(Qt::PlainText);
        meta->setToolTip(path);
        text->addWidget(name);
        text->addWidget(meta);
        line->addLayout(text, 1);
        auto* segment = new QHBoxLayout;
        segment->setSpacing(0);
        auto* group = new QButtonGroup(frame);
        group->setExclusive(true);
        for (const Option& option : kOptions) {
            auto* button = new QToolButton(frame);
            button->setObjectName(option.name);
            button->setText(option.text);
            button->setIcon(QIcon(option.icon));
            button->setIconSize(QSize(14, 14));
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setCheckable(true);
            button->setChecked(option.policy == OverwritePolicy::KeepBoth);
            group->addButton(button, static_cast<int>(option.policy));
            segment->addWidget(button);
        }
        connect(group, &QButtonGroup::idClicked, this, [this](int) { updateSummary(); });
        line->addLayout(segment);
        rows->addWidget(frame);
        m_groups.push_back(group);
    }
    rows->addStretch(1);
    scroll->setWidget(list);
    layout->addWidget(scroll, 1);

    auto* footer = new QHBoxLayout;
    m_summary = new QLabel(this);
    m_summary->setObjectName("conflictSummary");
    footer->addWidget(m_summary, 1);
    auto* cancel = new QPushButton("取消", this);
    cancel->setObjectName("cancelConflicts");
    auto* confirm = new QPushButton("按所选处理", this);
    confirm->setObjectName("confirmConflicts");
    confirm->setAutoDefault(false);
    cancel->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(confirm, &QPushButton::clicked, this, &QDialog::accept);
    footer->addWidget(cancel);
    footer->addWidget(confirm);
    layout->addLayout(footer);
    updateSummary();
}

zipdesk::OverwritePolicy ConflictDialog::choice(int row) const {
    return static_cast<OverwritePolicy>(m_groups[row]->checkedId());
}

void ConflictDialog::setChoice(int row, OverwritePolicy policy) {
    if (auto* button = m_groups[row]->button(static_cast<int>(policy))) button->setChecked(true);
    updateSummary();
}

void ConflictDialog::setAll(OverwritePolicy policy) {
    for (int row = 0; row < m_groups.size(); ++row) {
        if (auto* button = m_groups[row]->button(static_cast<int>(policy))) button->setChecked(true);
    }
    updateSummary();
}

std::map<std::wstring, zipdesk::OverwritePolicy> ConflictDialog::choices() const {
    std::map<std::wstring, OverwritePolicy> result;
    for (int row = 0; row < m_groups.size(); ++row) result[m_items[row].path] = choice(row);
    return result;
}

void ConflictDialog::updateSummary() {
    int keep = 0, skip = 0, overwrite = 0;
    for (int row = 0; row < m_groups.size(); ++row) {
        switch (choice(row)) {
        case OverwritePolicy::KeepBoth: ++keep; break;
        case OverwritePolicy::Skip: ++skip; break;
        case OverwritePolicy::Overwrite: ++overwrite; break;
        }
    }
    m_summary->setText(QString("保留两者 %1 · 跳过 %2 · 覆盖 %3").arg(keep).arg(skip).arg(overwrite));
}
