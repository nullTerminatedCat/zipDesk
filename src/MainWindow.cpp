#include "MainWindow.hpp"

#include "ArchiveJob.hpp"
#include "CompressDialog.hpp"
#include "ProgressDialog.hpp"
#include "Lang.hpp"
#include "ShellIntegration.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QCloseEvent>
#include <QEvent>
#include <QPainter>
#include <QShowEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QCheckBox>
#include <QButtonGroup>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QProcess>
#include <QRadioButton>
#include <QSettings>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QMimeData>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStackedWidget>
#include <QStyle>
#include <QToolTip>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

namespace {

constexpr auto kInternalMime = "application/x-zipdesk-internal";

QString humanSize(quint64 bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QString::number(bytes) + " B";
    }
    return QString::number(value, 'f', value >= 10.0 ? 1 : 2) + " " + units[unit];
}

QIcon gearIcon() {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#1f2430"));
    painter.translate(8, 8);
    for (int tooth = 0; tooth < 8; ++tooth) {
        painter.drawRoundedRect(QRectF(-1.15, -8, 2.3, 3.1), 0.6, 0.6);
        painter.rotate(45);
    }
    painter.resetTransform();
    painter.setBrush(QColor("#f6f1e8"));
    painter.drawEllipse(QRectF(4.3, 4.3, 7.4, 7.4));
    painter.setBrush(QColor("#1f2430"));
    painter.drawEllipse(QRectF(6.5, 6.5, 3, 3));
    return pixmap;
}

QIcon pinIcon(bool pinned) {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = pinned ? QColor("#0f6e6b") : QColor("#1f2430");
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(3.5, 12.5), QPointF(8, 8));
    painter.setBrush(pinned ? color : Qt::NoBrush);
    painter.drawEllipse(QRectF(6.4, 2.2, 6.4, 6.4));
    return pixmap;
}

class CaptionToolTip : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* button = qobject_cast<QToolButton*>(watched);
        if (button == nullptr || button->toolTip().isEmpty()) {
            return false;
        }
        if (event->type() == QEvent::Enter) {
            QTimer::singleShot(400, button, [button]() {
                if (button->underMouse()) {
                    QToolTip::showText(QCursor::pos(), button->toolTip(), button);
                }
            });
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
            QToolTip::hideText();
        }
        return false;
    }
};

QToolButton* captionButton(QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName("captionButton");
    button->setAutoRaise(true);
    button->setIconSize(QSize(16, 16));
    button->setFixedSize(36, 32);
    button->setMouseTracking(true);
    button->setAttribute(Qt::WA_Hover, true);
    button->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    static auto* tips = new CaptionToolTip;
    button->installEventFilter(tips);
    return button;
}

class SizeItem : public QStandardItem {
public:
    SizeItem(quint64 size, const QString& text) {
        setText(text);
        setData(static_cast<qulonglong>(size), Qt::UserRole);
        setEditable(false);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    bool operator<(const QStandardItem& other) const override {
        return data(Qt::UserRole).toULongLong() < other.data(Qt::UserRole).toULongLong();
    }
};

bool isInternalDrag(const QMimeData* mime) {
    return mime != nullptr && mime->hasFormat(QLatin1String(kInternalMime));
}

bool isExternalDrop(const QMimeData* mime) {
    return mime != nullptr && !isInternalDrag(mime) && mime->hasUrls();
}

void acceptFileDrag(QDragMoveEvent* event) {
    if (isExternalDrop(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

class ArchiveTableView : public QTableView {
public:
    std::function<void()> onDrag;
    std::function<void(const QList<QUrl>&)> onDrop;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override { acceptFileDrag(event); }
    void dragMoveEvent(QDragMoveEvent* event) override { acceptFileDrag(event); }
    void dropEvent(QDropEvent* event) override {
        if (!isExternalDrop(event->mimeData())) {
            event->ignore();
            return;
        }
        if (onDrop) {
            onDrop(event->mimeData()->urls());
        }
        event->acceptProposedAction();
    }
    void startDrag(Qt::DropActions) override {
        if (onDrag) {
            onDrag();
        }
    }
};

class DropLabel : public QLabel {
public:
    std::function<void(const QList<QUrl>&)> onDrop;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override { acceptFileDrag(event); }
    void dragMoveEvent(QDragMoveEvent* event) override { acceptFileDrag(event); }
    void dropEvent(QDropEvent* event) override {
        if (!isExternalDrop(event->mimeData())) {
            event->ignore();
            return;
        }
        if (onDrop) {
            onDrop(event->mimeData()->urls());
        }
        event->acceptProposedAction();
    }
};

class DragOutMime : public QMimeData {
public:
    std::function<QList<QUrl>()> produce;

    QStringList formats() const override {
        return {QLatin1String(kInternalMime), QStringLiteral("text/uri-list")};
    }

    bool hasFormat(const QString& mimeType) const override { return formats().contains(mimeType); }

protected:
    QVariant retrieveData(const QString& mimeType, QMetaType preferredType) const override {
        if (mimeType == QLatin1String(kInternalMime)) {
            return QByteArray("1");
        }
        if (mimeType != QLatin1String("text/uri-list")) {
            return {};
        }
        if (!m_ready && !m_producing) {
            m_producing = true;
            if (produce) {
                m_urls = produce();
            }
            m_ready = true;
            m_producing = false;
        }
        while (m_producing && !m_ready) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        }
        if (preferredType.id() == QMetaType::QVariantList) {
            return QVariant::fromValue(m_urls);
        }
        QByteArray encoded;
        for (const QUrl& url : m_urls) {
            encoded += url.toEncoded();
            encoded += "\r\n";
        }
        return encoded;
    }

private:
    mutable QList<QUrl> m_urls;
    mutable bool m_ready = false;
    mutable bool m_producing = false;
};

QStringList toStringList(const std::vector<std::wstring>& paths) {
    QStringList result;
    for (const std::wstring& path : paths) {
        result.push_back(QString::fromStdWString(path));
    }
    return result;
}

std::vector<std::wstring> relativePathsOf(const std::vector<zipdesk::ArchiveEntry>& entries,
                                           const std::vector<std::uint32_t>& indices) {
    std::vector<std::wstring> paths;
    for (const zipdesk::ArchiveEntry& entry : entries) {
        const bool take = indices.empty() ||
                          std::find(indices.begin(), indices.end(), entry.index) != indices.end();
        if (take) {
            paths.push_back(entry.path);
        }
    }
    return paths;
}


std::wstring namedFolderFor(const std::wstring& archivePath, bool* created) {
    const std::filesystem::path archive(archivePath);
    const std::filesystem::path folder = archive.parent_path() / archive.stem();
    std::error_code error;
    if (std::filesystem::exists(folder, error) && !std::filesystem::is_directory(folder, error)) {
        throw zipdesk::ArchiveError(zipdesk::ErrorKind::Other, L"同名文件已存在，无法创建解压文件夹。");
    }
    const bool existed = std::filesystem::exists(folder, error);
    std::filesystem::create_directories(folder);
    if (created != nullptr) {
        *created = !existed;
    }
    return folder.wstring();
}

}  // namespace

MainWindow::MainWindow(zipdesk::ArchiveService& service, zipdesk::LaunchCommand launch, QWidget* parent)
    : QMainWindow(parent)
    , m_service(service)
    , m_launch(std::move(launch))
    , m_job(new ArchiveJob(this))
    , m_progress(new ProgressDialog(this)) {
    buildUi();
    m_job->setPasswordPrompt([this](const QString& prompt) { return promptPassword(prompt); });
    connect(m_job, &ArchiveJob::totalChanged, m_progress, &ProgressDialog::setTotal);
    connect(m_job, &ArchiveJob::progressChanged, m_progress, &ProgressDialog::setProcessed);
    connect(m_job, &ArchiveJob::fileChanged, m_progress, &ProgressDialog::setCurrentFile);
    connect(m_job, &ArchiveJob::succeeded, this, &MainWindow::onSucceeded);
    connect(m_job, &ArchiveJob::failed, this, &MainWindow::onFailed);
    connect(m_job, &ArchiveJob::cancelled, this, &MainWindow::onCancelled);
    connect(m_progress, &ProgressDialog::cancelRequested, m_job, &ArchiveJob::requestCancel);
    QTimer::singleShot(0, this, &MainWindow::runLaunch);
}

QToolButton* makeToolButton(QToolBar* toolbar, const QString& text, bool primary) {
    auto* button = new QToolButton(toolbar);
    button->setProperty("i18n", text);
    button->setText(zipdesk::translate(text));
    button->setToolTip(zipdesk::translate(text));
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(false);
    button->setObjectName(primary ? "primaryButton" : "toolButton");
    toolbar->addWidget(button);
    return button;
}

void MainWindow::buildUi() {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    m_stayOnTop = QSettings().value(QStringLiteral("ui/stayOnTop"), false).toBool();
    setWindowTitle("日常解压");
    setWindowIcon(QIcon(":/assets/icon.png"));
    resize(980, 680);
    setStyleSheet(
        "QMainWindow, QDialog { background: #f3efe6; color: #1f2430; }"
        "QToolBar { background: transparent; border: none; spacing: 4px; padding: 8px 12px; }"
        "QWidget#titleBar { background: #f6f1e8; border: none; border-bottom: 1px solid #eadfd2; }"
        "QLabel#captionTitle { font-size: 13px; color: #1f2430; padding-left: 8px; }"
        "QToolButton#captionButton { background: transparent; border: none; border-radius: 6px; padding: 0; }"
        "QToolButton#captionButton:hover { background: #e7ddd0; }"
        "QToolButton#captionButton:checked { background: #d7ebe8; }"
        "QToolButton#closeButton { background: transparent; border: none; border-radius: 6px; padding: 0; }"
        "QToolButton#closeButton:hover { background: #c75050; }"
        "QToolButton#primaryButton, QPushButton#primaryButton { background: #0f6e6b; color: white; border: none; border-radius: 10px; padding: 8px 16px; }"
        "QToolButton#primaryButton:hover, QPushButton#primaryButton:hover { background: #0c5c59; }"
        "QToolButton#primaryButton:disabled { background: #b7c9c7; color: #f7f4ee; }"
        "QToolButton#toolButton, QPushButton { background: #fffdf8; color: #1f2430; border: 1px solid #e4d8c8; border-radius: 8px; padding: 4px 8px; }"
        "QToolButton#toolButton:hover, QPushButton:hover { border-color: #0f6e6b; }"
        "QToolButton:disabled, QPushButton:disabled { color: #b3a89c; }"
        "QLabel#brandTitle { font-size: 18px; font-weight: 600; color: #1f2430; }"
        "QLabel#emptyTitle { font-size: 22px; font-weight: 600; color: #1f2430; }"
        "QLabel#emptyBody { font-size: 13px; color: #6d645b; }"
        "QFrame#archiveCard { background: #fffdf8; border: 1px solid #eadfD2; border-radius: 16px; }"
        "QTableView { background: transparent; border: none; alternate-background-color: #faf6f0; "
        "selection-background-color: #d7ebe8; selection-color: #1f2430; outline: none; }"
        "QHeaderView::section { background: #fffdf8; color: #6d645b; padding: 10px 12px; border: none; "
        "border-bottom: 1px solid #eadfd2; font-weight: 600; }"
        "QStatusBar { background: transparent; color: #6d645b; }"
        "QMenu { background: #fffdf8; border: 1px solid #eadfd2; padding: 6px; }"
        "QMenu::item { padding: 6px 18px; border-radius: 6px; }"
        "QMenu::item:selected { background: #d7ebe8; }"
        "QLineEdit, QComboBox, QListWidget { background: #fffdf8; border: 1px solid #e4d8c8; border-radius: 8px; padding: 6px; }"
        "QProgressBar { border: none; background: #efe6da; border-radius: 5px; min-height: 8px; }"
        "QProgressBar::chunk { background: #0f6e6b; border-radius: 5px; }");

    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName("titleBar");
    m_titleBar->setFixedHeight(36);
    m_titleBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* captionLayout = new QHBoxLayout(m_titleBar);
    captionLayout->setContentsMargins(10, 0, 0, 0);
    captionLayout->setSpacing(0);
    auto* captionIcon = new QLabel(m_titleBar);
    captionIcon->setPixmap(QIcon(":/assets/icon.png").pixmap(18, 18));
    captionIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    captionLayout->addWidget(captionIcon);
    m_captionTitle = new QLabel("日常解压", m_titleBar);
    m_captionTitle->setObjectName("captionTitle");
    m_captionTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    captionLayout->addWidget(m_captionTitle);
    captionLayout->addStretch(1);
    m_pinButton = captionButton(m_titleBar);
    m_pinButton->setCheckable(true);
    m_pinButton->setChecked(m_stayOnTop);
    m_pinButton->setIcon(pinIcon(m_stayOnTop));
    m_pinButton->setToolTip(m_stayOnTop ? "取消钉住" : "钉在前台");
    connect(m_pinButton, &QToolButton::clicked, this, &MainWindow::toggleStayOnTop);
    captionLayout->addWidget(m_pinButton);
    auto* settingsButton = captionButton(m_titleBar);
    settingsButton->setIcon(gearIcon());
    settingsButton->setProperty("tip", "设置");
    settingsButton->setToolTip(zipdesk::translate("设置"));
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::showSettings);
    captionLayout->addWidget(settingsButton);
    auto* minButton = captionButton(m_titleBar);
    minButton->setProperty("tip", "最小化");
    minButton->setToolTip(zipdesk::translate("最小化"));
    minButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarMinButton));
    connect(minButton, &QToolButton::clicked, this, &QWidget::showMinimized);
    captionLayout->addWidget(minButton);
    m_maxButton = captionButton(m_titleBar);
    connect(m_maxButton, &QToolButton::clicked, this, [this]() {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    captionLayout->addWidget(m_maxButton);
    auto* closeButton = captionButton(m_titleBar);
    closeButton->setProperty("tip", "关闭");
    closeButton->setToolTip(zipdesk::translate("关闭"));
    closeButton->setObjectName("closeButton");
    closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    captionLayout->addWidget(closeButton);
    setMenuWidget(m_titleBar);
    connect(this, &QWidget::windowTitleChanged, this, [this](const QString& title) {
        if (m_captionTitle != nullptr) {
            m_captionTitle->setText(title);
        }
    });
    updateCaptionButtons();

    auto* toolbar = addToolBar("主工具栏");
    m_toolbar = toolbar;
    toolbar->setMovable(false);

    m_homeButton = makeToolButton(toolbar, "返回", false);
    m_homeButton->setVisible(false);
    connect(m_homeButton, &QToolButton::clicked, this, &MainWindow::returnHome);

    m_openButton = makeToolButton(toolbar, "打开", true);
    m_openAction = new QAction("打开", this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::chooseAndOpen);
    connect(m_openButton, &QToolButton::clicked, m_openAction, &QAction::trigger);
    addAction(m_openAction);

    m_newButton = makeToolButton(toolbar, "新建压缩", false);
    m_newAction = new QAction("新建压缩", this);
    connect(m_newAction, &QAction::triggered, this, [this]() { createArchive({}); });
    connect(m_newButton, &QToolButton::clicked, m_newAction, &QAction::trigger);

    m_extractMenu = new QMenu(this);
    m_extractMenu->addAction("解压到…", this, [this]() { extractArchive(false, false); });
    m_extractMenu->addAction("解压到同名文件夹", this, [this]() { extractArchive(false, true); });
    m_extractSelectedAction = m_extractMenu->addAction("解压所选到…", this, [this]() { extractArchive(true, false); });
    m_extractSelectedFolderAction =
        m_extractMenu->addAction("解压所选到同名文件夹", this, [this]() { extractArchive(true, true); });
    m_extractButton = makeToolButton(toolbar, "解压", false);
    m_extractButton->setMenu(m_extractMenu);
    m_extractButton->setPopupMode(QToolButton::InstantPopup);

    m_appendMenu = new QMenu(this);
    m_appendMenu->addAction("追加文件", this, [this]() {
        const QStringList files = QFileDialog::getOpenFileNames(this, "追加文件");
        if (!files.isEmpty()) {
            appendPaths(files);
        }
    });
    m_appendMenu->addAction("追加文件夹", this, [this]() {
        const QString folder = QFileDialog::getExistingDirectory(this, "追加文件夹");
        if (!folder.isEmpty()) {
            appendPaths({folder});
        }
    });
    m_testButton = makeToolButton(toolbar, "测试", false);
    m_repairButton = makeToolButton(toolbar, "修复", false);
    connect(m_repairButton, &QToolButton::clicked, this, &MainWindow::repairArchive);
    connect(m_testButton, &QToolButton::clicked, this, &MainWindow::testArchive);
    m_copyButton = makeToolButton(toolbar, "复制", false);
    connect(m_copyButton, &QToolButton::clicked, this, &MainWindow::copySelectedTo);
    m_moveButton = makeToolButton(toolbar, "移动", false);
    connect(m_moveButton, &QToolButton::clicked, this, &MainWindow::moveSelectedTo);
    m_deleteButton = makeToolButton(toolbar, "删除", false);
    connect(m_deleteButton, &QToolButton::clicked, this, &MainWindow::deleteSelected);
    m_infoButton = makeToolButton(toolbar, "信息", false);
    connect(m_infoButton, &QToolButton::clicked, this, &MainWindow::showInfo);
    m_appendButton = makeToolButton(toolbar, "追加", false);
    m_appendButton->setMenu(m_appendMenu);
    m_appendButton->setPopupMode(QToolButton::InstantPopup);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({"名称", "原大小", "压缩后大小", "修改时间", "类型"});
    auto* table = new ArchiveTableView;
    table->setModel(m_model);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setDragEnabled(true);
    table->setAcceptDrops(true);
    table->setDragDropMode(QAbstractItemView::DragDrop);
    table->setDropIndicatorShown(false);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setFrameShape(QFrame::NoFrame);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(36);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setMinimumSectionSize(56);
    table->horizontalHeader()->resizeSection(0, 420);
    table->horizontalHeader()->resizeSection(1, 110);
    table->horizontalHeader()->resizeSection(2, 120);
    table->horizontalHeader()->resizeSection(3, 160);
    table->horizontalHeader()->resizeSection(4, 72);
    table->setSortingEnabled(true);
    table->onDrag = [this]() { dragOutSelection(); };
    table->onDrop = [this](const QList<QUrl>& urls) { handleDropped(urls); };
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QWidget::customContextMenuRequested, this, &MainWindow::showItemMenu);
    m_table = table;

    auto* card = new QFrame;
    card->setObjectName("archiveCard");
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(8, 8, 8, 8);
    cardLayout->addWidget(table);
    m_archivePage = new QWidget;
    auto* archiveLayout = new QVBoxLayout(m_archivePage);
    archiveLayout->setContentsMargins(20, 8, 20, 16);
    m_previewStack = new QStackedWidget;
    auto* previewHint = new QLabel("选择文件可预览文本或图片");
    previewHint->setAlignment(Qt::AlignCenter);
    previewHint->setWordWrap(true);
    previewHint->setProperty("i18n", "选择文件可预览文本或图片");
    m_previewText = new QPlainTextEdit;
    m_previewText->setReadOnly(true);
    m_previewImage = new QLabel;
    m_previewImage->setAlignment(Qt::AlignCenter);
    m_previewStack->addWidget(previewHint);
    m_previewStack->addWidget(m_previewText);
    m_previewStack->addWidget(m_previewImage);
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(card);
    splitter->addWidget(m_previewStack);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    archiveLayout->addWidget(splitter);
    connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        updatePreview();
        updateActions();
    });

    auto* empty = new DropLabel;
    empty->setAcceptDrops(true);
    empty->onDrop = [this](const QList<QUrl>& urls) { handleDropped(urls); };
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setAlignment(Qt::AlignCenter);
    emptyLayout->setSpacing(8);
    auto* picture = new QLabel(empty);
    picture->setAlignment(Qt::AlignCenter);
    picture->setPixmap(QPixmap(":/assets/empty.png").scaled(520, 292, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    picture->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* emptyTitle = new QLabel("把压缩包放在这里", empty);
    emptyTitle->setObjectName("emptyTitle");
    emptyTitle->setProperty("i18n", "把压缩包放在这里");
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* emptyBody = new QLabel("拖入 zip / 7z 即可打开，拖入文件或文件夹则新建压缩", empty);
    emptyBody->setObjectName("emptyBody");
    emptyBody->setProperty("i18n", "拖入 zip / 7z 即可打开，拖入文件或文件夹则新建压缩");
    emptyBody->setAlignment(Qt::AlignCenter);
    emptyBody->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* emptyButtons = new QHBoxLayout;
    emptyButtons->setAlignment(Qt::AlignCenter);
    auto* emptyOpen = new QPushButton("打开压缩包", empty);
    emptyOpen->setObjectName("primaryButton");
    auto* emptyNew = new QPushButton("新建压缩", empty);
    emptyButtons->addWidget(emptyOpen);
    emptyButtons->addWidget(emptyNew);
    emptyLayout->addWidget(picture);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyBody);
    emptyLayout->addSpacing(8);
    emptyLayout->addLayout(emptyButtons);
    connect(emptyOpen, &QPushButton::clicked, this, &MainWindow::chooseAndOpen);
    connect(emptyNew, &QPushButton::clicked, this, [this]() { createArchive({}); });
    m_emptyPage = empty;

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_emptyPage);
    m_stack->addWidget(m_archivePage);
    setCentralWidget(m_stack);
    statusBar()->showMessage("未打开压缩包");
    installItemShortcuts();
    applyLanguage();
    updateActions();
}

void MainWindow::updateCaptionButtons() {
    if (m_maxButton == nullptr) {
        return;
    }
    m_maxButton->setIcon(style()->standardIcon(isMaximized() ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
    m_maxButton->setToolTip(zipdesk::translate(isMaximized() ? "还原" : "最大化"));
}

void MainWindow::toggleStayOnTop() {
    m_stayOnTop = !m_stayOnTop;
    QSettings().setValue(QStringLiteral("ui/stayOnTop"), m_stayOnTop);
    if (m_pinButton != nullptr) {
        m_pinButton->setChecked(m_stayOnTop);
        m_pinButton->setIcon(pinIcon(m_stayOnTop));
        m_pinButton->setToolTip(zipdesk::translate(m_stayOnTop ? "取消钉住" : "钉在前台"));
    }
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    SetWindowPos(hwnd, m_stayOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (m_frameReady) {
        return;
    }
    m_frameReady = true;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
    SetWindowLongW(hwnd, GWL_STYLE, style);
    const MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (m_stayOnTop) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateCaptionButtons();
    }
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (!m_frameReady || eventType != "windows_generic_MSG") {
        return QMainWindow::nativeEvent(eventType, message, result);
    }
    auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_NCCALCSIZE && msg->wParam) {
        if (IsZoomed(msg->hwnd)) {
            MONITORINFO info{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
                reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam)->rgrc[0] = info.rcWork;
            }
        }
        *result = 0;
        return true;
    }
    if (msg->message == WM_NCHITTEST && m_titleBar != nullptr) {
        const LONG x = GET_X_LPARAM(msg->lParam);
        const LONG y = GET_Y_LPARAM(msg->lParam);
        RECT windowRect{};
        GetWindowRect(msg->hwnd, &windowRect);
        const int border = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        if (!IsZoomed(msg->hwnd)) {
            const bool left = x < windowRect.left + border;
            const bool right = x >= windowRect.right - border;
            const bool top = y < windowRect.top + border;
            const bool bottom = y >= windowRect.bottom - border;
            if (top && left) {
                *result = HTTOPLEFT;
                return true;
            }
            if (top && right) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (bottom && left) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (bottom && right) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (left) {
                *result = HTLEFT;
                return true;
            }
            if (right) {
                *result = HTRIGHT;
                return true;
            }
            if (top) {
                *result = HTTOP;
                return true;
            }
            if (bottom) {
                *result = HTBOTTOM;
                return true;
            }
        }
        const QPoint local = m_titleBar->mapFromGlobal(QPoint(x, y));
        if (m_titleBar->rect().contains(local)) {
            if (qobject_cast<QAbstractButton*>(m_titleBar->childAt(local)) != nullptr) {
                *result = HTCLIENT;
            } else {
                *result = HTCAPTION;
            }
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_job->isRunning()) {
        m_closeAfterJob = true;
        m_job->requestCancel();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::runLaunch() {
    if (m_launch.paths.empty() && m_launch.action != zipdesk::LaunchAction::None &&
        m_launch.action != zipdesk::LaunchAction::Open) {
        return;
    }
    switch (m_launch.action) {
    case zipdesk::LaunchAction::Open:
        openArchive(QString::fromStdWString(m_launch.paths.front()));
        break;
    case zipdesk::LaunchAction::ExtractHere:
        shellExtract(false);
        break;
    case zipdesk::LaunchAction::ExtractToFolder:
        shellExtract(true);
        break;
    case zipdesk::LaunchAction::CompressZip:
        shellCompress(zipdesk::ArchiveFormat::Zip);
        break;
    case zipdesk::LaunchAction::Compress7z:
        shellCompress(zipdesk::ArchiveFormat::SevenZip);
        break;
    case zipdesk::LaunchAction::None:
        break;
    }
}

void MainWindow::setBarItemVisible(QWidget* widget, bool visible) {
    if (widget == nullptr || m_toolbar == nullptr) {
        return;
    }
    widget->setVisible(visible);
    for (QAction* action : m_toolbar->actions()) {
        if (m_toolbar->widgetForAction(action) == widget) {
            action->setVisible(visible);
            return;
        }
    }
}

void MainWindow::updateActions() {
    const bool open = !m_path.empty();
    const bool hasSelection = open && !selectedIndices().empty();
    setBarItemVisible(m_openButton, !open);
    setBarItemVisible(m_newButton, !open);
    setBarItemVisible(m_homeButton, open);
    setBarItemVisible(m_testButton, open);
    setBarItemVisible(m_repairButton, open);
    setBarItemVisible(m_copyButton, open);
    setBarItemVisible(m_moveButton, open);
    setBarItemVisible(m_deleteButton, open);
    setBarItemVisible(m_infoButton, open);
    setBarItemVisible(m_appendButton, open);
    setBarItemVisible(m_extractButton, !open);
    m_homeButton->setEnabled(open && !m_busy);
    m_testButton->setEnabled(open && !m_busy);
    m_repairButton->setEnabled(open && !m_busy);
    m_appendButton->setEnabled(open && !m_busy);
    m_copyButton->setEnabled(hasSelection && !m_busy);
    m_moveButton->setEnabled(hasSelection && !m_busy);
    m_deleteButton->setEnabled(hasSelection && !m_busy);
    m_infoButton->setEnabled(hasSelection && !m_busy);
    m_extractButton->setEnabled(!m_busy);
    if (m_extractSelectedAction != nullptr) {
        m_extractSelectedAction->setVisible(open);
    }
    if (m_extractSelectedFolderAction != nullptr) {
        m_extractSelectedFolderAction->setVisible(open);
    }
    m_openAction->setEnabled(!m_busy);
    m_newAction->setEnabled(!m_busy);
    for (QAction* action : m_itemActions) {
        action->setEnabled(open && !m_busy);
    }
}

void MainWindow::toggleShellIntegration() {
    const std::wstring exe = QCoreApplication::applicationFilePath().toStdWString();
    if (zipdesk::shellIntegrationInstalled(exe)) {
        zipdesk::removeShellIntegration();
        statusBar()->showMessage("已取消资源管理器集成");
    } else {
        zipdesk::installShellIntegration(exe);
        statusBar()->showMessage("已关联 .zip / .7z，右键菜单在「显示更多选项」里");
    }
    updateActions();
}

void MainWindow::setBusy(bool busy) {
    m_busy = busy;
    updateActions();
}

void MainWindow::showArchive(zipdesk::ArchiveInfo info) {
    m_path = info.path;
    if (!info.password.empty()) {
        m_password = info.password;
    }
    m_encrypted = info.encrypted || !m_password.empty();
    m_entries = std::move(info.entries);
    m_comment = info.comment;
    m_prefix.clear();
    fillModel();
    m_stack->setCurrentWidget(m_archivePage);
    const QString name = QFileInfo(QString::fromStdWString(m_path)).fileName();
    setWindowTitle(name + " - 日常解压");
    statusBar()->showMessage(QString("%1 项").arg(m_entries.size()));
    updateActions();
}

void MainWindow::fillModel() {
    m_table->setSortingEnabled(false);
    m_model->removeRows(0, m_model->rowCount());
    if (!m_prefix.empty()) {
        auto* up = new QStandardItem("..");
        up->setData(0xFFFFFFFFu, Qt::UserRole);
        up->setData(true, Qt::UserRole + 2);
        up->setEditable(false);
        QList<QStandardItem*> upRow;
        upRow << up << new QStandardItem << new QStandardItem << new QStandardItem << new QStandardItem(QStringLiteral("目录"));
        for (auto* item : upRow) {
            item->setEditable(false);
        }
        m_model->appendRow(upRow);
    }
    for (const zipdesk::ArchiveEntry& entry : m_entries) {
        if (!m_prefix.empty() && (entry.path == m_prefix || !zipdesk::pathInside(m_prefix, entry.path))) {
            continue;
        }
        const QString path = QString::fromStdWString(entry.path).replace('/', '\\');
        auto* name = new QStandardItem(path.isEmpty() ? QString::fromStdWString(entry.name) : path);
        name->setData(entry.index, Qt::UserRole);
        name->setEditable(false);
        name->setDragEnabled(true);
        name->setDropEnabled(false);

        QList<QStandardItem*> row;
        row << name;
        row << new SizeItem(entry.isDir ? 0 : entry.size, entry.isDir ? QString("—") : humanSize(entry.size));
        row << new SizeItem(entry.isDir ? 0 : entry.packSize, entry.isDir ? QString("—") : humanSize(entry.packSize));
        auto* modified = new QStandardItem(entry.hasModified ? QString::fromStdWString(entry.modified) : QString());
        modified->setEditable(false);
        row << modified;
        auto* type = new QStandardItem(entry.isDir ? "目录" : "文件");
        type->setEditable(false);
        row << type;
        m_model->appendRow(row);
    }
    m_table->setSortingEnabled(true);
}

void MainWindow::storePending(zipdesk::ArchiveInfo info) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pending = std::move(info);
    m_hasPending = true;
}

zipdesk::ArchiveInfo MainWindow::takePending() {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_hasPending = false;
    return std::move(m_pending);
}

void MainWindow::begin(const QString& title,
                       std::function<void(const zipdesk::OperationHooks&)> task,
                       std::function<void()> onSuccess) {
    if (m_job->isRunning()) {
        return;
    }
    m_onSuccess = std::move(onSuccess);
    m_progress->reset(title);
    m_progress->setWindowModality(Qt::ApplicationModal);
    m_progress->show();
    m_progress->raise();
    setBusy(true);
    m_job->start(std::move(task));
}

void MainWindow::onSucceeded() {
    m_progress->hide();
    setBusy(false);
    if (m_inDrag) {
        m_dragDone = true;
        m_dragOk = true;
        return;
    }
    if (m_closeAfterJob) {
        close();
        return;
    }
    auto callback = std::move(m_onSuccess);
    m_onSuccess = nullptr;
    if (callback) {
        callback();
    }
}

void MainWindow::onFailed(const QString& message) {
    m_progress->hide();
    setBusy(false);
    m_onSuccess = nullptr;
    if (m_inDrag) {
        m_dragDone = true;
        m_dragOk = false;
        m_dragError = message;
        return;
    }
    if (m_closeAfterJob) {
        close();
        return;
    }
    QMessageBox::warning(this, "日常解压", message);
}

void MainWindow::onCancelled() {
    m_progress->hide();
    setBusy(false);
    m_onSuccess = nullptr;
    if (m_inDrag) {
        m_dragDone = true;
        m_dragOk = false;
        return;
    }
    statusBar()->showMessage("已取消");
    if (m_closeAfterJob) {
        close();
    }
}

std::optional<QString> MainWindow::promptPassword(const QString& text) {
    QInputDialog dialog(m_progress->isVisible() ? static_cast<QWidget*>(m_progress) : static_cast<QWidget*>(this));
    dialog.setWindowTitle("密码");
    dialog.setLabelText(text);
    dialog.setTextEchoMode(QLineEdit::Password);
    dialog.setOkButtonText("确定");
    dialog.setCancelButtonText("取消");
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    m_password = dialog.textValue().toStdWString();
    return dialog.textValue();
}

std::optional<zipdesk::OverwritePolicy> MainWindow::askOverwrite() {
    QMessageBox box(m_progress->isVisible() ? static_cast<QWidget*>(m_progress) : static_cast<QWidget*>(this));
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("同名文件");
    box.setText("目标位置已有同名文件。这一批要如何处理？");
    auto* overwrite = box.addButton("全部覆盖", QMessageBox::AcceptRole);
    auto* skip = box.addButton("全部跳过", QMessageBox::ActionRole);
    box.addButton("取消", QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == overwrite) {
        return zipdesk::OverwritePolicy::Overwrite;
    }
    if (box.clickedButton() == skip) {
        return zipdesk::OverwritePolicy::Skip;
    }
    return std::nullopt;
}

std::vector<std::uint32_t> MainWindow::selectedIndices() const {
    std::vector<std::uint32_t> indices;
    if (m_table->selectionModel() == nullptr) {
        return indices;
    }
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    indices.reserve(static_cast<std::size_t>(rows.size()));
    for (const QModelIndex& index : rows) {
        if (index.data(Qt::UserRole + 2).toBool()) {
            continue;
        }
        indices.push_back(index.data(Qt::UserRole).toUInt());
    }
    return indices;
}

void MainWindow::chooseAndOpen() {
    const QString path = QFileDialog::getOpenFileName(this, "打开压缩包", QString(), "压缩包 (*.zip *.7z *.rar)");
    if (!path.isEmpty()) {
        openArchive(path);
    }
}

void MainWindow::openArchive(const QString& path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, "日常解压", "找不到压缩包。");
        return;
    }
    if (!zipdesk::isArchivePath(path.toStdWString())) {
        QMessageBox::warning(this, "日常解压", "只支持 zip 和 7z。");
        return;
    }
    const bool sameArchive = m_path == path.toStdWString();
    const std::wstring password = sameArchive ? m_password : L"";
    if (!sameArchive) {
        m_password.clear();
    }
    begin(
        "正在打开",
        [this, path, password](const zipdesk::OperationHooks& hooks) {
            storePending(m_service.read(path.toStdWString(), password, hooks));
        },
        [this]() { showArchive(takePending()); });
}

void MainWindow::createArchive(const QStringList& inputs) {
    CompressDialog dialog(this);
    zipdesk::CompressRequest seed;
    seed.format = zipdesk::ArchiveFormat::Zip;
    seed.level = zipdesk::CompressionLevel::Normal;
    for (const QString& input : inputs) {
        seed.inputs.push_back(input.toStdWString());
    }
    dialog.setRequest(seed);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const zipdesk::CompressRequest request = dialog.request();
    const QString output = QString::fromStdWString(request.outputPath);
    if (QFileInfo::exists(output)) {
        const auto answer = QMessageBox::question(this, "日常解压", output + " 已存在，要覆盖吗？");
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    begin(
        "正在压缩",
        [this, request](const zipdesk::OperationHooks& hooks) { m_service.compress(request, hooks); },
        [this, output]() { finishCompress(output); });
}

int MainWindow::compressFinishMode() const {
    return QSettings().value(QStringLiteral("compress/finishMode"), 0).toInt() == 1 ? 1 : 0;
}

void MainWindow::finishCompress(const QString& output) {
    const QString native = QDir::toNativeSeparators(QFileInfo(output).absoluteFilePath());
    if (compressFinishMode() == 0) {
        QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + native});
        statusBar()->showMessage("已生成 " + native);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("压缩完成");
    dialog.setModal(true);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(18);
    auto* text = new QLabel("已生成\n" + native, &dialog);
    text->setAlignment(Qt::AlignCenter);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* openFile = new QPushButton("打开文件", &dialog);
    openFile->setObjectName("primaryButton");
    auto* close = new QPushButton("关闭", &dialog);
    buttons->addWidget(openFile);
    buttons->addWidget(close);
    buttons->addStretch();
    layout->addLayout(buttons);
    dialog.resize(440, 180);
    connect(openFile, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(1); });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() == 1) {
        openArchive(output);
    }
}

void MainWindow::returnHome() {
    if (m_job->isRunning()) {
        return;
    }
    m_path.clear();
    m_password.clear();
    m_entries.clear();
    m_prefix.clear();
    m_comment.clear();
    m_encrypted = false;
    m_model->removeRows(0, m_model->rowCount());
    m_stack->setCurrentWidget(m_emptyPage);
    setWindowTitle("日常解压");
    statusBar()->showMessage("未打开压缩包");
    updateActions();
}

void MainWindow::showSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle("设置");
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(12);
    auto* title = new QLabel("压缩完成后", &dialog);
    layout->addWidget(title);
    auto* explorer = new QRadioButton("在资源管理器中打开所在目录，并选中压缩包", &dialog);
    auto* prompt = new QRadioButton("弹出提示，可选择打开文件或关闭", &dialog);
    if (compressFinishMode() == 1) {
        prompt->setChecked(true);
    } else {
        explorer->setChecked(true);
    }
    layout->addWidget(explorer);
    layout->addWidget(prompt);
    layout->addSpacing(8);
    layout->addWidget(new QLabel("界面语言", &dialog));
    auto* chinese = new QRadioButton("简体中文", &dialog);
    auto* english = new QRadioButton("English", &dialog);
    if (zipdesk::uiLanguage() == zipdesk::UiLanguage::English) {
        english->setChecked(true);
    } else {
        chinese->setChecked(true);
    }
    layout->addWidget(chinese);
    layout->addWidget(english);
    layout->addSpacing(8);
    layout->addWidget(new QLabel("资源管理器", &dialog));
    const std::wstring exe = QCoreApplication::applicationFilePath().toStdWString();
    auto* shell = new QCheckBox("集成到资源管理器（关联 zip、7z、rar，并加入右键菜单）", &dialog);
    shell->setChecked(zipdesk::shellIntegrationInstalled(exe));
    layout->addWidget(shell);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* ok = new QPushButton("确定", &dialog);
    ok->setObjectName("primaryButton");
    auto* cancel = new QPushButton("取消", &dialog);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.resize(520, 400);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const bool integrated = zipdesk::shellIntegrationInstalled(QCoreApplication::applicationFilePath().toStdWString());
    if (shell->isChecked() != integrated) {
        toggleShellIntegration();
    }
    QSettings().setValue(QStringLiteral("compress/finishMode"), prompt->isChecked() ? 1 : 0);
    zipdesk::setUiLanguage(english->isChecked() ? zipdesk::UiLanguage::English : zipdesk::UiLanguage::Chinese);
    applyLanguage();
}

void MainWindow::applyLanguage() {
    const auto refresh = [](QWidget* widget) {
        const QString key = widget->property("i18n").toString();
        if (key.isEmpty()) {
            return;
        }
        if (auto* button = qobject_cast<QToolButton*>(widget)) {
            const QString text = zipdesk::translate(key);
            if (button->objectName() != QLatin1String("captionButton") && button->objectName() != QLatin1String("closeButton")) {
                button->setText(text);
            }
            button->setToolTip(text);
        } else if (auto* label = qobject_cast<QLabel*>(widget)) {
            label->setText(zipdesk::translate(key));
        }
    };
    for (QWidget* widget : findChildren<QWidget*>()) {
        refresh(widget);
    }
    if (m_path.empty()) {
        setWindowTitle(zipdesk::uiLanguage() == zipdesk::UiLanguage::English ? "ZipDesk" : "日常解压");
    }
    if (m_pinButton != nullptr) {
        m_pinButton->setToolTip(zipdesk::translate(m_stayOnTop ? "取消钉住" : "钉在前台"));
    }
    for (QToolButton* button : findChildren<QToolButton*>()) {
        const QString tip = button->property("tip").toString();
        if (!tip.isEmpty()) {
            button->setToolTip(zipdesk::translate(tip));
        }
    }
    updateCaptionButtons();
}

void MainWindow::appendPaths(const QStringList& paths) {
    if (m_path.empty() || paths.isEmpty()) {
        return;
    }
    zipdesk::AppendRequest request;
    request.archivePath = m_path;
    request.password = m_password;
    request.level = zipdesk::CompressionLevel::Normal;
    for (const QString& path : paths) {
        request.inputs.push_back(path.toStdWString());
    }
    const QString archive = QString::fromStdWString(m_path);
    begin(
        "正在追加",
        [this, request](const zipdesk::OperationHooks& hooks) { m_service.append(request, hooks); },
        [this, archive]() { openArchive(archive); });
}

void MainWindow::extractArchive(bool selectedOnly, bool namedFolder) {
    if (m_path.empty()) {
        if (selectedOnly) {
            return;
        }
        const QString chosen = QFileDialog::getOpenFileName(this, "选择要解压的压缩包", QString(), "压缩包 (*.zip *.7z *.rar)");
        if (chosen.isEmpty()) {
            return;
        }
        const std::wstring archive = chosen.toStdWString();
        std::wstring outputDir;
        bool created = false;
        if (namedFolder) {
            try {
                outputDir = namedFolderFor(archive, &created);
            } catch (const zipdesk::ArchiveError& error) {
                QMessageBox::warning(this, "日常解压", QString::fromStdWString(error.message()));
                return;
            }
        } else {
            const QString picked = QFileDialog::getExistingDirectory(this, "解压到", QFileInfo(chosen).absolutePath());
            if (picked.isEmpty()) {
                return;
            }
            outputDir = picked.toStdWString();
        }
        const QString finishedDir = QString::fromStdWString(outputDir);
        begin(
            "正在解压",
            [this, archive, outputDir, created](const zipdesk::OperationHooks& hooks) {
                const zipdesk::ArchiveInfo info = m_service.read(archive, L"", hooks);
                zipdesk::OverwritePolicy policy = zipdesk::OverwritePolicy::Overwrite;
                if (zipdesk::hasConflicts(info.entries, {}, outputDir)) {
                    std::optional<zipdesk::OverwritePolicy> choice;
                    QMetaObject::invokeMethod(
                        this, [&]() { choice = askOverwrite(); }, Qt::BlockingQueuedConnection);
                    if (!choice) {
                        if (created) {
                            std::error_code error;
                            std::filesystem::remove_all(outputDir, error);
                        }
                        throw zipdesk::ArchiveError(zipdesk::ErrorKind::Cancelled, L"已取消");
                    }
                    policy = *choice;
                }
                zipdesk::ExtractRequest request;
                request.archivePath = archive;
                request.outputDir = outputDir;
                request.password = info.password;
                request.overwrite = policy;
                request.removeOutputOnCancel = created;
                request.relativePaths = relativePathsOf(info.entries, {});
                m_service.extract(request, hooks);
            },
            [this, finishedDir]() {
                statusBar()->showMessage("已解压到 " + finishedDir);
                QMessageBox::information(this, "日常解压", "解压完成。\n" + finishedDir);
            });
        return;
    }
    if (m_entries.empty()) {
        QMessageBox::information(this, "日常解压", "这个压缩包是空的。");
        return;
    }
    std::vector<std::uint32_t> indices;
    if (selectedOnly) {
        indices = selectedIndices();
        if (indices.empty()) {
            QMessageBox::information(this, "日常解压", "请先选择要解压的项目。");
            return;
        }
        indices = zipdesk::expandSelection(m_entries, indices);
    }

    std::wstring outputDir;
    bool removeOnCancel = false;
    if (namedFolder) {
        try {
            outputDir = namedFolderFor(m_path, &removeOnCancel);
        } catch (const zipdesk::ArchiveError& error) {
            QMessageBox::warning(this, "日常解压", QString::fromStdWString(error.message()));
            return;
        }
    } else {
        const QString picked = QFileDialog::getExistingDirectory(
            this, "解压到", QFileInfo(QString::fromStdWString(m_path)).absolutePath());
        if (picked.isEmpty()) {
            return;
        }
        outputDir = picked.toStdWString();
    }

    zipdesk::OverwritePolicy policy = zipdesk::OverwritePolicy::Overwrite;
    if (zipdesk::hasConflicts(m_entries, indices, outputDir)) {
        const auto choice = askOverwrite();
        if (!choice) {
            if (removeOnCancel) {
                std::error_code error;
                std::filesystem::remove_all(outputDir, error);
            }
            return;
        }
        policy = *choice;
    }

    zipdesk::ExtractRequest request;
    request.archivePath = m_path;
    request.outputDir = outputDir;
    request.password = m_password;
    request.indices = indices;
    request.relativePaths = relativePathsOf(m_entries, indices);
    request.overwrite = policy;
    request.removeOutputOnCancel = removeOnCancel;
    const QString finishedDir = QString::fromStdWString(outputDir);
    begin(
        "正在解压",
        [this, request](const zipdesk::OperationHooks& hooks) { m_service.extract(request, hooks); },
        [this, finishedDir]() {
            statusBar()->showMessage("已解压到 " + finishedDir);
            QMessageBox::information(this, "日常解压", "解压完成。\n" + finishedDir);
        });
}

void MainWindow::shellExtract(bool namedFolder) {
    if (m_launch.paths.empty()) {
        return;
    }
    const std::wstring archive = m_launch.paths.front();
    begin(
        "正在解压",
        [this, archive, namedFolder](const zipdesk::OperationHooks& hooks) {
            const zipdesk::ArchiveInfo info = m_service.read(archive, L"", hooks);
            storePending(info);
            bool created = false;
            std::wstring outputDir;
            if (namedFolder) {
                outputDir = namedFolderFor(archive, &created);
            } else {
                outputDir = std::filesystem::path(archive).parent_path().wstring();
            }
            zipdesk::OverwritePolicy policy = zipdesk::OverwritePolicy::Overwrite;
            if (zipdesk::hasConflicts(info.entries, {}, outputDir)) {
                std::optional<zipdesk::OverwritePolicy> choice;
                QMetaObject::invokeMethod(
                    this, [&]() { choice = askOverwrite(); }, Qt::BlockingQueuedConnection);
                if (!choice) {
                    if (created) {
                        std::error_code error;
                        std::filesystem::remove_all(outputDir, error);
                    }
                    throw zipdesk::ArchiveError(zipdesk::ErrorKind::Cancelled, L"已取消");
                }
                policy = *choice;
            }
            zipdesk::ExtractRequest request;
            request.archivePath = archive;
            request.outputDir = outputDir;
            request.password = info.password;
            request.overwrite = policy;
            request.removeOutputOnCancel = created;
            request.relativePaths = relativePathsOf(info.entries, {});
            m_service.extract(request, hooks);
        },
        [this]() {
            showArchive(takePending());
            QMessageBox::information(this, "日常解压", "解压完成。");
        });
}

void MainWindow::shellCompress(zipdesk::ArchiveFormat format) {
    if (m_launch.paths.empty()) {
        return;
    }
    QStringList sources;
    for (const std::wstring& path : m_launch.paths) {
        const QString source = QString::fromStdWString(path);
        if (QFileInfo::exists(source)) {
            sources.push_back(source);
        }
    }
    if (sources.isEmpty()) {
        QMessageBox::warning(this, "日常解压", "找不到要压缩的文件。");
        return;
    }
    const QString extension = format == zipdesk::ArchiveFormat::Zip ? ".zip" : ".7z";
    const QString output = defaultArchivePath(sources, extension);
    if (QFileInfo::exists(output)) {
        const auto answer = QMessageBox::question(this, "日常解压", output + " 已存在，要覆盖吗？");
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    zipdesk::CompressRequest request;
    for (const QString& source : sources) {
        request.inputs.push_back(source.toStdWString());
    }
    request.outputPath = output.toStdWString();
    request.format = format;
    request.level = zipdesk::CompressionLevel::Normal;
    begin(
        "正在压缩",
        [this, request](const zipdesk::OperationHooks& hooks) { m_service.compress(request, hooks); },
        [this, output]() {
            QMessageBox::information(this, "日常解压", "压缩完成。");
            openArchive(output);
        });
}

void MainWindow::handleDropped(const QList<QUrl>& urls) {
    if (m_busy || m_job->isRunning()) {
        return;
    }
    std::vector<std::wstring> paths;
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile().toStdWString());
        }
    }
    const zipdesk::DropDecision decision = zipdesk::decideDrop(paths, !m_path.empty());
    switch (decision.action) {
    case zipdesk::DropAction::OpenArchive:
        openArchive(QString::fromStdWString(decision.archivePath));
        break;
    case zipdesk::DropAction::AppendFiles:
        appendPaths(toStringList(decision.files));
        break;
    case zipdesk::DropAction::CreateArchive:
        if (!decision.files.empty()) {
            createArchive(toStringList(decision.files));
        }
        break;
    }
}

void MainWindow::dragOutSelection() {
    if (m_path.empty() || m_job->isRunning()) {
        return;
    }
    const std::vector<std::uint32_t> selected = selectedIndices();
    if (selected.empty()) {
        return;
    }
    if (m_encrypted && m_password.empty()) {
        const auto answer = promptPassword("拖出加密文件前，请先输入密码。");
        if (!answer) {
            return;
        }
        statusBar()->showMessage("密码已记录，请把选中的文件再拖一次。");
        return;
    }

    const QString temp = QDir(QDir::tempPath())
                             .filePath(QString("ZipDesk/drag-%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
    QDir().mkpath(temp);
    const std::vector<std::uint32_t> indices = zipdesk::expandSelection(m_entries, selected);

    auto* mime = new DragOutMime;
    mime->produce = [this, temp, indices]() { return produceDragUrls(temp, indices); };

    m_inDrag = true;
    m_dragDone = false;
    m_dragOk = false;
    m_dragError.clear();
    QDrag drag(m_table);
    drag.setMimeData(mime);
    drag.exec(Qt::CopyAction);
    m_inDrag = false;
    QDir(temp).removeRecursively();
    if (!m_dragError.isEmpty()) {
        const QString message = m_dragError;
        m_dragError.clear();
        if (message.contains("密码")) {
            m_password.clear();
        }
        QMessageBox::warning(this, "日常解压", message);
    }
}

QList<QUrl> MainWindow::produceDragUrls(const QString& tempDir, const std::vector<std::uint32_t>& indices) {
    zipdesk::ExtractRequest request;
    request.archivePath = m_path;
    request.outputDir = tempDir.toStdWString();
    request.password = m_password;
    request.indices = indices;
    request.relativePaths = relativePathsOf(m_entries, indices);
    request.overwrite = zipdesk::OverwritePolicy::Overwrite;
    request.removeOutputOnCancel = true;

    m_dragDone = false;
    m_dragOk = false;
    m_progress->reset("正在准备拖出");
    m_progress->setWindowModality(Qt::NonModal);
    m_progress->setAttribute(Qt::WA_ShowWithoutActivating, true);
    m_progress->show();
    m_job->start([this, request](const zipdesk::OperationHooks& hooks) { m_service.extract(request, hooks); });
    while (!m_dragDone) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    }
    m_progress->hide();
    if (!m_dragOk) {
        return {};
    }

    QList<QUrl> urls;
    const std::vector<std::uint32_t> selected = selectedIndices();
    for (const std::uint32_t index : selected) {
        const auto entry = std::find_if(m_entries.begin(), m_entries.end(), [index](const zipdesk::ArchiveEntry& item) {
            return item.index == index;
        });
        if (entry == m_entries.end()) {
            continue;
        }
        bool covered = false;
        for (const std::uint32_t otherIndex : selected) {
            if (otherIndex == index) {
                continue;
            }
            const auto other = std::find_if(m_entries.begin(), m_entries.end(), [otherIndex](const zipdesk::ArchiveEntry& item) {
                return item.index == otherIndex;
            });
            if (other != m_entries.end() && other->isDir && zipdesk::pathInside(other->path, entry->path)) {
                covered = true;
                break;
            }
        }
        if (covered) {
            continue;
        }
        const QString path = QDir(tempDir).filePath(QString::fromStdWString(entry->path));
        if (QFileInfo::exists(path)) {
            urls.push_back(QUrl::fromLocalFile(path));
        }
    }
    return urls;
}
