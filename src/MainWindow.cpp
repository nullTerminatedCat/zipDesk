#include "MainWindow.hpp"

#include "ArchiveJob.hpp"
#include "CompressDialog.hpp"
#include "ProgressDialog.hpp"
#include "Lang.hpp"
#include "ShellIntegration.hpp"

#include <QAbstractButton>
#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QCheckBox>
#include <QClipboard>
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
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QProcess>
#include <QRadioButton>
#include <QSettings>
#include <QScrollArea>
#include <QShortcut>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QMimeData>
#include <QSet>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QStatusBar>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QToolTip>
#include <QTableView>
#include <QThread>
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

// Keep long archive names from dictating the window's minimum width.
class ElidedLabel : public QLabel {
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return {0, QLabel::minimumSizeHint().height()}; }
    QSize sizeHint() const override { return {160, QLabel::sizeHint().height()}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.setFont(font());
        painter.drawText(contentsRect(), Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()));
    }
};

class RecentArchiveDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return {200, 62}; }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = QIcon();
        const QStyle* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
        painter->save();
        const QRect rect = option.rect.adjusted(14, 8, -14, -8);
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole)).paint(painter, QRect(rect.left(), rect.top() + 11, 24, 24));
        const QRect title = rect.adjusted(38, 0, 0, -22);
        QFont font = option.font;
        font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        painter->setPen(QColor("#20332f"));
        painter->drawText(title, Qt::AlignVCenter, QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideMiddle, title.width()));
        font.setWeight(QFont::Normal);
        painter->setFont(font);
        painter->setPen(QColor("#667b75"));
        const QRect path = rect.adjusted(38, 24, 0, 0);
        painter->drawText(path, Qt::AlignVCenter, QFontMetrics(font).elidedText(index.data(Qt::UserRole + 1).toString(), Qt::ElideMiddle, path.width()));
        painter->restore();
    }
};

class HomeScrollArea : public QScrollArea {
public:
    std::function<void(int)> onWidthChanged;
protected:
    bool viewportEvent(QEvent* event) override {
        const bool result = QScrollArea::viewportEvent(event);
        if (event->type() == QEvent::Resize && onWidthChanged) onWidthChanged(viewport()->width());
        return result;
    }
};

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

QIcon toolGlyph(const QString& name, const QColor& color = QColor("#3a6762")) {
    QPixmap pixmap(40, 40);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (name == QStringLiteral("首页")) {
        painter.drawLine(QPointF(3, 9), QPointF(10, 3));
        painter.drawLine(QPointF(10, 3), QPointF(17, 9));
        painter.drawRoundedRect(QRectF(5, 8, 10, 9), 1.5, 1.5);
        painter.drawLine(QPointF(9, 17), QPointF(9, 12));
        painter.drawLine(QPointF(9, 12), QPointF(12, 12));
    } else if (name == QStringLiteral("打开")) {
        painter.drawRoundedRect(QRectF(2.5, 6, 15, 11), 2, 2);
        painter.drawLine(QPointF(3.5, 6), QPointF(3.5, 4));
        painter.drawLine(QPointF(3.5, 4), QPointF(9, 4));
        painter.drawLine(QPointF(9, 4), QPointF(11, 6));
    } else if (name == QStringLiteral("新建压缩")) {
        painter.drawRoundedRect(QRectF(4, 2.5, 11, 15), 1.5, 1.5);
        painter.drawLine(QPointF(7, 7), QPointF(12, 7));
        painter.drawLine(QPointF(9.5, 11), QPointF(9.5, 15));
        painter.drawLine(QPointF(7.5, 13), QPointF(11.5, 13));
    } else if (name == QStringLiteral("解压")) {
        painter.drawRoundedRect(QRectF(3, 12.5, 14, 4.5), 1.2, 1.2);
        painter.drawLine(QPointF(10, 2.5), QPointF(10, 11));
        painter.drawLine(QPointF(6.5, 8), QPointF(10, 11.5));
        painter.drawLine(QPointF(13.5, 8), QPointF(10, 11.5));
    } else if (name == QStringLiteral("添加")) {
        painter.drawRoundedRect(QRectF(2.5, 4, 15, 12.5), 2, 2);
        painter.drawLine(QPointF(10, 7), QPointF(10, 13.5));
        painter.drawLine(QPointF(6.8, 10.2), QPointF(13.2, 10.2));
    } else if (name == QStringLiteral("工具")) {
        painter.setBrush(color);
        for (int x : {5, 10, 15}) painter.drawEllipse(QPointF(x, 10), 1.35, 1.35);
    } else if (name == QStringLiteral("预览")) {
        QPainterPath eye;
        eye.moveTo(2.2, 10);
        eye.cubicTo(6.2, 4, 13.8, 4, 17.8, 10);
        eye.cubicTo(13.8, 16, 6.2, 16, 2.2, 10);
        painter.drawPath(eye);
        painter.drawEllipse(QPointF(10, 10), 2.3, 2.3);
    } else if (name == QStringLiteral("上一级")) {
        painter.drawLine(QPointF(10, 16), QPointF(10, 4));
        painter.drawLine(QPointF(5.5, 8.5), QPointF(10, 4));
        painter.drawLine(QPointF(14.5, 8.5), QPointF(10, 4));
    }
    painter.end();
    return pixmap;
}

QIcon archiveItemGlyph(bool folder) {
    QPixmap pixmap(38, 38);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#3b8b7d"), 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(QColor("#e4f4ef"));
    if (folder) {
        painter.drawRoundedRect(QRectF(1.5, 5.5, 16, 11), 2.2, 2.2);
        painter.drawLine(QPointF(2.5, 5.7), QPointF(2.5, 3.8));
        painter.drawLine(QPointF(2.5, 3.8), QPointF(8.5, 3.8));
    } else {
        painter.drawRoundedRect(QRectF(4, 1.5, 11, 16), 1.7, 1.7);
        painter.drawLine(QPointF(7, 8), QPointF(12, 8));
        painter.drawLine(QPointF(7, 11), QPointF(12, 11));
    }
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
    button->setFixedSize(36, 36);
    button->setMouseTracking(true);
    button->setAttribute(Qt::WA_Hover, true);
    button->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    static auto* tips = new CaptionToolTip;
    button->installEventFilter(tips);
    return button;
}


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
    std::function<void(bool)> onDragActive;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::RightButton || selectionModel() == nullptr) {
            QTableView::mousePressEvent(event);
            return;
        }
        const QModelIndex target = indexAt(event->pos());
        if (!target.isValid() || !selectionModel()->isRowSelected(target.row(), QModelIndex())) {
            QTableView::mousePressEvent(event);
            return;
        }
        const QItemSelection selection = selectionModel()->selection();
        QTableView::mousePressEvent(event);
        if (selectionModel()->selection() != selection) {
            selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
        }
        selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        acceptFileDrag(event);
        if (event->isAccepted() && onDragActive) {
            onDragActive(true);
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override { acceptFileDrag(event); }
    void dragLeaveEvent(QDragLeaveEvent* event) override {
        if (onDragActive) {
            onDragActive(false);
        }
        QTableView::dragLeaveEvent(event);
    }
    void dropEvent(QDropEvent* event) override {
        if (onDragActive) {
            onDragActive(false);
        }
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
    std::function<void(bool)> onDragActive;
    std::function<void(int)> onResize;

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        if (onResize) onResize(width());
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        acceptFileDrag(event);
        if (event->isAccepted() && onDragActive) {
            onDragActive(true);
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override { acceptFileDrag(event); }
    void dragLeaveEvent(QDragLeaveEvent* event) override {
        if (onDragActive) {
            onDragActive(false);
        }
        QLabel::dragLeaveEvent(event);
    }
    void dropEvent(QDropEvent* event) override {
        if (onDragActive) {
            onDragActive(false);
        }
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

class ArchiveListModel final : public QAbstractTableModel {
public:
    struct Row {
        QString name;
        QString path;
        QString modified;
        quint64 size = 0;
        quint64 packed = 0;
        std::uint32_t index = 0;
        bool directory = false;
        bool virtualDirectory = false;
        bool parent = false;
    };

    explicit ArchiveListModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent)
        , m_folderIcon(archiveItemGlyph(true))
        , m_fileIcon(archiveItemGlyph(false)) {}

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 5; }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!index.isValid()) return Qt::NoItemFlags;
        Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!m_rows[static_cast<std::size_t>(index.row())].parent) result |= Qt::ItemIsDragEnabled;
        return result;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        static const QStringList headers = {"名称", "原大小", "压缩后大小", "修改时间", "类型"};
        return section >= 0 && section < headers.size() ? QVariant(headers[section]) : QVariant();
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() >= 5) return {};
        const Row& row = m_rows[static_cast<std::size_t>(index.row())];
        if (role == Qt::UserRole + 6) {
            if (index.column() == 1) return static_cast<qulonglong>(row.size);
            if (index.column() == 2) return static_cast<qulonglong>(row.packed);
        }
        if (index.column() == 0) {
            if (role == Qt::UserRole) return row.index;
            if (role == Qt::UserRole + 2) return row.parent;
            if (role == Qt::UserRole + 3) return row.path;
            if (role == Qt::UserRole + 4) return row.virtualDirectory;
            if (role == Qt::UserRole + 5) return row.directory;
            if (role == Qt::DecorationRole) return row.directory ? m_folderIcon : m_fileIcon;
            if (role == Qt::ToolTipRole) return row.path;
        }
        if (role == Qt::TextAlignmentRole && (index.column() == 1 || index.column() == 2)) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (role != Qt::DisplayRole) return {};
        switch (index.column()) {
        case 0: return row.name;
        case 1: return row.directory ? QString("—") : humanSize(row.size);
        case 2: return row.directory ? QString("—") : humanSize(row.packed);
        case 3: return row.modified;
        case 4: return row.directory ? QString("目录") : QString("文件");
        default: return {};
        }
    }

    void setRows(std::vector<Row> rows) {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }

private:
    std::vector<Row> m_rows;
    QIcon m_folderIcon;
    QIcon m_fileIcon;
};

class EmptyIllustration : public QWidget {
public:
    explicit EmptyIllustration(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(244, 190);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(width() / 244.0, height() / 190.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#eaf7f2"));
        painter.drawEllipse(QRectF(30, 4, 182, 182));
        painter.setBrush(QColor("#c9ebe0"));
        painter.drawEllipse(QRectF(23, 57, 12, 12));
        painter.drawEllipse(QRectF(204, 123, 8, 8));

        painter.setBrush(QColor(15, 110, 107, 28));
        painter.drawRoundedRect(QRectF(58, 118, 132, 18), 9, 9);
        painter.setPen(QPen(QColor("#e4d8c8"), 1.2));
        painter.setBrush(QColor("#fffdf8"));
        painter.drawRoundedRect(QRectF(52, 58, 140, 78), 16, 16);
        painter.drawRoundedRect(QRectF(48, 36, 148, 36), 14, 14);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#0f6e6b"));
        painter.drawRoundedRect(QRectF(112, 28, 18, 108), 8, 8);
        painter.drawEllipse(QRectF(108, 68, 26, 26));
    }
};

class ArchiveSortProxy final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        if (left.column() == 0) {
            const bool leftParent = left.data(Qt::UserRole + 2).toBool();
            const bool rightParent = right.data(Qt::UserRole + 2).toBool();
            if (leftParent != rightParent) return leftParent;
            const bool leftDir = left.data(Qt::UserRole + 5).toBool();
            const bool rightDir = right.data(Qt::UserRole + 5).toBool();
            if (leftDir != rightDir) return leftDir;
        }
        if (left.column() == 1 || left.column() == 2) {
            const auto a = left.data(Qt::UserRole + 6).toULongLong();
            const auto b = right.data(Qt::UserRole + 6).toULongLong();
            if (a != b) return a < b;
        }
        return QString::localeAwareCompare(left.data().toString(), right.data().toString()) < 0;
    }
};

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

MainWindow::~MainWindow() {
    if (m_previewThread != nullptr) {
        if (m_previewCancel) m_previewCancel->store(true);
        m_previewThread->requestInterruption();
        m_previewThread->wait();
    }
}

QToolButton* makeToolButton(QToolBar* toolbar, const QString& text, bool primary) {
    auto* button = new QToolButton(toolbar);
    button->setProperty("i18n", text);
    button->setText(zipdesk::translate(text));
    button->setToolTip(zipdesk::translate(text));
    button->setIcon(toolGlyph(text));
    button->setIconSize(QSize(20, 20));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(false);
    button->setObjectName(primary ? "primaryButton" : "toolButton");
    toolbar->addWidget(button);
    return button;
}

void MainWindow::buildUi() {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setMinimumSize(720, 520);
    m_stayOnTop = QSettings().value(QStringLiteral("ui/stayOnTop"), false).toBool();
    setWindowTitle("青匣");
    setWindowIcon(QIcon(":/assets/icon.png"));
    setFont(QFont("Microsoft YaHei UI", 9));
    resize(980, 680);
    setStyleSheet(
        "QWidget { font-size: 13px; }"
        "QLabel#dropHint { background: rgba(22, 116, 104, 0.14); color: #0f6e6b; border: 2px dashed #167468; "
        "border-radius: 16px; font-size: 18px; font-weight: 700; margin: 18px; }"
        "QMainWindow, QDialog { background: #f5f7fa; color: #20332f; }"
        "QToolBar { background: #ffffff; border: none; border-bottom: 1px solid #e3eae7; spacing: 8px; padding: 10px 20px; }"
        "QWidget#titleBar { background: #ffffff; border: none; border-bottom: 1px solid #e0eae6; }"
        "QLabel#captionTitle { font-size: 13px; font-weight: 600; color: #213c38; padding-left: 9px; }"
        "QToolButton#captionButton, QToolButton#closeButton { background: transparent; border: none; border-radius: 7px; padding: 0; }"
        "QToolButton#captionButton:hover { background: #eaf2ef; }"
        "QToolButton#captionButton:checked { background: #dff1eb; }"
        "QToolButton#closeButton:hover { background: #fbe6e5; }"
        "QToolButton#primaryButton, QPushButton#primaryButton { background: #167468; color: white; border: 1px solid #167468; border-radius: 9px; padding: 9px 16px; font-weight: 600; }"
        "QToolButton#primaryButton:hover, QPushButton#primaryButton:hover { background: #0e6258; border-color: #0e6258; }"
        "QToolButton#primaryButton:disabled, QPushButton#primaryButton:disabled { background: #a9c9bf; border-color: #a9c9bf; color: #f4f8f6; }"
        "QToolButton#toolButton, QPushButton { background: #ffffff; color: #294b45; border: 1px solid #d9e6e1; border-radius: 9px; padding: 8px 13px; }"
        "QToolButton#toolButton:hover, QPushButton:hover { background: #f6fbf9; border-color: #8abbb0; }"
        "QToolButton#toolButton:checked { background: #e0f3ed; border-color: #8abbb0; }"
        "QToolButton#toolButton:focus, QToolButton#primaryButton:focus, QPushButton:focus { border: 1px solid #147d70; background: #e2f3ed; color: #174e45; }"
        "QToolButton:disabled, QPushButton:disabled { color: #9aada7; }"
        "QPushButton#quietButton { background: transparent; border: none; color: #167468; padding: 5px 7px; }"
        "QPushButton#quietButton:hover { background: #e7f4ef; }"
        "QFrame#heroCard { background: #123f39; border: 1px solid #123f39; border-radius: 16px; }"
        "QFrame#archiveCard { background: #ffffff; border: 1px solid #dfe7e4; border-radius: 12px; }"
        "QLabel#heroEyebrow { color: #9bcec0; font-size: 12px; font-weight: 600; }"
        "QLabel#emptyTitle { color: #ffffff; font-size: 26px; font-weight: 700; }"
        "QLabel#emptyBody { color: #c2d8d1; font-size: 13px; }"
        "QPushButton#homeOpen { background: #d8f2e8; color: #123f39; border: 1px solid #d8f2e8; padding: 11px 18px; font-weight: 600; }"
        "QPushButton#homeOpen:hover { background: #effbf6; }"
        "QPushButton#homeOpen:focus { border: 2px solid #ffffff; padding: 10px 17px; }"
        "QPushButton#homeNew { background: #20534a; color: #ffffff; border: 1px solid #558278; padding: 11px 18px; }"
        "QPushButton#homeNew:hover, QPushButton#homeNew:focus { background: #2d675d; border-color: #c2d8d1; }"
        "QLabel#recentTitle, QLabel#fileHeading { color: #20332f; font-size: 15px; font-weight: 600; }"
        "QLabel#archiveSummary, QLabel#homeHint { color: #667b75; font-size: 12px; }"
        "QLabel#emptyResultTitle { color: #20332f; font-size: 18px; font-weight: 600; }"
        "QLabel#emptyResultBody { color: #667b75; }"
        "QLabel#archiveLocation { color: #35534d; font-size: 13px; font-weight: 600; }"
        "QToolButton#locationUp { background: white; border: 1px solid #d9e6e1; border-radius: 9px; }"
        "QToolButton#locationUp:hover { border-color: #8abbb0; background: #f6fbf9; }"
        "QToolButton#locationUp:disabled { background: #f2f6f4; border-color: #e8efec; }"
        "QTableView { background: white; color: #20332f; border: none; alternate-background-color: #f8faf9; selection-background-color: #e0f2eb; selection-color: #123f39; }"
        "QTableView::item { padding: 0 8px; border: none; }"
        "QTableView::item:focus { border: 1px solid #79ac9e; }"
        "QHeaderView::section { background: #fbfdfc; color: #657e76; padding: 11px 12px; border: none; border-bottom: 1px solid #e3ece8; font-weight: 600; }"
        "QStatusBar { background: #ffffff; color: #667b75; border-top: 1px solid #e4ece9; padding: 3px 10px; }"
        "QStatusBar::item { border: none; }"
        "QMenu { background: #ffffff; color: #294b45; border: 1px solid #dfeae5; padding: 5px; }"
        "QMenu::item { padding: 7px 20px; border-radius: 5px; }"
        "QMenu::item:selected { background: #e6f4ef; }"
        "QLineEdit, QComboBox, QListWidget { background: #ffffff; color: #294b45; border: 1px solid #d9e6e1; border-radius: 9px; padding: 7px 9px; selection-background-color: #d8eee7; }"
        "QLineEdit:focus, QComboBox:focus, QListWidget:focus { border-color: #4aa18f; }"
        "QListWidget#recentArchives { padding: 4px; }"
        "QListWidget#recentArchives::item { border-radius: 6px; }"
        "QListWidget#recentArchives::item:hover { background: #f0f6f3; }"
        "QListWidget#recentArchives::item:selected { background: #e0f2eb; }"
        "QPlainTextEdit { background: white; color: #20332f; border: 1px solid #dfe7e4; border-radius: 8px; padding: 8px; }"
        "QSplitter::handle { background: transparent; width: 10px; }"
        "QProgressBar { border: none; background: #e6efeb; border-radius: 5px; min-height: 8px; }"
        "QProgressBar::chunk { background: #167468; border-radius: 5px; }");

    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName("titleBar");
    m_titleBar->setFixedHeight(44);
    m_titleBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* captionLayout = new QHBoxLayout(m_titleBar);
    captionLayout->setContentsMargins(10, 0, 0, 0);
    captionLayout->setSpacing(0);
    auto* captionIcon = new QLabel(m_titleBar);
    captionIcon->setPixmap(QIcon(":/assets/icon.png").pixmap(18, 18));
    captionIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    captionLayout->addWidget(captionIcon);
    m_captionTitle = new ElidedLabel("青匣", m_titleBar);
    m_captionTitle->setObjectName("captionTitle");
    m_captionTitle->setMinimumWidth(245);
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

    m_homeButton = makeToolButton(toolbar, "首页", false);
    m_homeButton->setVisible(false);
    connect(m_homeButton, &QToolButton::clicked, this, &MainWindow::returnHome);

    m_openButton = makeToolButton(toolbar, "打开", false);
    m_openAction = new QAction("打开", this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::chooseAndOpen);
    connect(m_openButton, &QToolButton::clicked, m_openAction, &QAction::trigger);
    addAction(m_openAction);

    m_newButton = makeToolButton(toolbar, "新建压缩", false);
    m_newAction = new QAction("新建压缩", this);
    m_newAction->setShortcut(QKeySequence::New);
    addAction(m_newAction);
    connect(m_newAction, &QAction::triggered, this, [this]() { createArchive({}); });
    connect(m_newButton, &QToolButton::clicked, m_newAction, &QAction::trigger);

    m_extractMenu = new QMenu(this);
    m_extractMenu->addAction("智能解压", this, [this]() {
        const QString path = m_path.empty()
            ? QFileDialog::getOpenFileName(this, "选择要解压的压缩包", QString(), "压缩包 (*.zip *.7z *.rar)")
            : QString::fromStdWString(m_path);
        if (!path.isEmpty()) smartExtract(path);
    });
    m_extractMenu->addAction("解压到…", this, [this]() { extractArchive(false, false); });
    m_extractMenu->addAction("解压到同名文件夹", this, [this]() { extractArchive(false, true); });
    m_extractSelectedAction = m_extractMenu->addAction("解压所选到…", this, [this]() { extractArchive(true, false); });
    m_extractSelectedFolderAction =
        m_extractMenu->addAction("解压所选到同名文件夹", this, [this]() { extractArchive(true, true); });
    m_extractButton = makeToolButton(toolbar, "解压", true);
    m_extractButton->setIcon(toolGlyph("解压", Qt::white));
    m_extractButton->setMenu(m_extractMenu);
    m_extractButton->setPopupMode(QToolButton::InstantPopup);

    m_appendMenu = new QMenu(this);
    m_appendMenu->addAction("添加文件", this, [this]() {
        const QStringList files = QFileDialog::getOpenFileNames(this, "追加文件");
        if (!files.isEmpty()) {
            appendPaths(files);
        }
    });
    m_appendMenu->addAction("添加文件夹", this, [this]() {
        const QString folder = QFileDialog::getExistingDirectory(this, "追加文件夹");
        if (!folder.isEmpty()) {
            appendPaths({folder});
        }
    });
    m_appendButton = makeToolButton(toolbar, "添加", false);
    m_appendButton->setMenu(m_appendMenu);
    m_appendButton->setPopupMode(QToolButton::InstantPopup);
    m_moreMenu = new QMenu(this);
    m_moreTestAction = m_moreMenu->addAction("测试完整性", this, &MainWindow::testArchive);
    m_moreRepairAction = m_moreMenu->addAction("抢救可读取文件", this, &MainWindow::repairArchive);
    m_moreMenu->addSeparator();
    m_moreCopyAction = m_moreMenu->addAction("复制所选到…", this, &MainWindow::copySelectedTo);
    m_moreMoveAction = m_moreMenu->addAction("移动所选到…", this, &MainWindow::moveSelectedTo);
    m_moreDeleteAction = m_moreMenu->addAction("删除所选", this, &MainWindow::deleteSelected);
    m_moreMenu->addSeparator();
    m_moreInfoAction = m_moreMenu->addAction("信息", this, &MainWindow::showInfo);
    m_moreButton = makeToolButton(toolbar, "工具", false);
    m_moreButton->setMenu(m_moreMenu);
    m_moreButton->setPopupMode(QToolButton::InstantPopup);
    for (QMenu* menu : {m_extractMenu, m_appendMenu, m_moreMenu}) {
        for (QAction* action : menu->actions()) {
            if (!action->isSeparator()) {
                action->setProperty("i18n", action->text());
            }
        }
    }
    m_previewButton = new QToolButton;
    m_previewButton->setObjectName("toolButton");
    m_previewButton->setProperty("i18n", "预览");
    m_previewButton->setText(zipdesk::translate("预览"));
    m_previewButton->setToolTip(zipdesk::translate("预览"));
    m_previewButton->setIcon(toolGlyph("预览"));
    m_previewButton->setIconSize(QSize(20, 20));
    m_previewButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_previewButton->setCheckable(true);
    m_previewButton->setChecked(QSettings().value(QStringLiteral("ui/previewVisible"), false).toBool());

    m_model = new ArchiveListModel(this);
    auto* sortedModel = new ArchiveSortProxy(this);
    sortedModel->setSourceModel(m_model);
    sortedModel->setDynamicSortFilter(false);
    auto* table = new ArchiveTableView;
    table->setModel(sortedModel);
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
    table->verticalHeader()->setDefaultSectionSize(40);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setMinimumSectionSize(56);
    table->horizontalHeader()->resizeSection(0, 420);
    table->horizontalHeader()->resizeSection(1, 110);
    table->horizontalHeader()->resizeSection(2, 120);
    table->horizontalHeader()->resizeSection(3, 160);
    table->horizontalHeader()->resizeSection(4, 72);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setSortingEnabled(true);
    table->sortByColumn(0, Qt::AscendingOrder);
    table->onDrag = [this]() { dragOutSelection(); };
    table->onDrop = [this](const QList<QUrl>& urls) { handleDropped(urls); };
    table->onDragActive = [this](bool active) { showDropHint(active); };
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QWidget::customContextMenuRequested, this, &MainWindow::showItemMenu);
    m_table = table;

    auto* card = new QFrame;
    card->setObjectName("archiveCard");
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(8, 8, 8, 8);
    auto* fileHeading = new QHBoxLayout;
    fileHeading->setContentsMargins(12, 8, 12, 10);
    auto* fileTitle = new QLabel("压缩包内容", card);
    fileTitle->setProperty("i18n", "压缩包内容");
    fileTitle->setObjectName("fileHeading");
    m_archiveSummary = new QLabel(card);
    m_archiveSummary->setObjectName("archiveSummary");
    fileHeading->addWidget(fileTitle);
    fileHeading->addStretch();
    fileHeading->addWidget(m_archiveSummary);
    cardLayout->addLayout(fileHeading);
    m_fileStack = new QStackedWidget(card);
    m_fileStack->addWidget(table);
    auto* emptyResult = new DropLabel;
    emptyResult->setAcceptDrops(true);
    emptyResult->onDrop = [this](const QList<QUrl>& urls) { handleDropped(urls); };
    emptyResult->onDragActive = [this](bool active) { showDropHint(active); };
    emptyResult->setObjectName("archiveEmptyState");
    auto* emptyResultLayout = new QVBoxLayout(emptyResult);
    emptyResultLayout->setSpacing(12);
    emptyResultLayout->addStretch();
    m_emptyResultTitle = new QLabel(emptyResult);
    m_emptyResultTitle->setObjectName("emptyResultTitle");
    m_emptyResultTitle->setAlignment(Qt::AlignCenter);
    m_emptyResultBody = new QLabel(emptyResult);
    m_emptyResultBody->setObjectName("emptyResultBody");
    m_emptyResultBody->setAlignment(Qt::AlignCenter);
    m_emptyResultBody->setWordWrap(true);
    auto* clearSearch = new QPushButton("清除搜索", emptyResult);
    clearSearch->setObjectName("clearArchiveSearch");
    clearSearch->setProperty("i18n", "清除搜索");
    m_clearSearchButton = clearSearch;
    emptyResultLayout->addWidget(m_emptyResultTitle);
    emptyResultLayout->addWidget(m_emptyResultBody);
    emptyResultLayout->addWidget(clearSearch, 0, Qt::AlignHCenter);
    emptyResultLayout->addStretch();
    m_fileStack->addWidget(emptyResult);
    cardLayout->addWidget(m_fileStack, 1);
    m_archivePage = new QWidget;
    auto* archiveLayout = new QVBoxLayout(m_archivePage);
    archiveLayout->setContentsMargins(20, 8, 20, 16);
    auto* locationBar = new QHBoxLayout;
    m_upButton = new QToolButton;
    m_upButton->setObjectName("locationUp");
    m_upButton->setIcon(toolGlyph("上一级"));
    m_upButton->setIconSize(QSize(20, 20));
    m_upButton->setToolTip("上一级 (Alt+↑)");
    m_upButton->setFixedSize(36, 36);
    connect(m_upButton, &QToolButton::clicked, this, &MainWindow::navigateUp);
    m_locationLabel = new ElidedLabel;
    m_locationLabel->setObjectName("archiveLocation");
    m_locationLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_locationLabel->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_locationLabel, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        QMenu menu(this);
        menu.addAction(zipdesk::translate("复制压缩包路径"), this, [this]() {
            QApplication::clipboard()->setText(QDir::toNativeSeparators(QString::fromStdWString(m_path)));
        });
        menu.exec(m_locationLabel->mapToGlobal(point));
    });
    m_searchEdit = new QLineEdit;
    m_searchEdit->setObjectName("archiveSearch");
    m_searchEdit->setPlaceholderText("搜索文件或路径…");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumWidth(160);
    m_searchEdit->setMaximumWidth(280);
    connect(clearSearch, &QPushButton::clicked, m_searchEdit, &QLineEdit::clear);
    auto* findAction = new QShortcut(QKeySequence::Find, this);
    connect(findAction, &QShortcut::activated, this, [this]() {
        if (!m_path.empty() && !m_busy) { m_searchEdit->setFocus(); m_searchEdit->selectAll(); }
    });
    auto* clearFind = new QShortcut(QKeySequence(Qt::Key_Escape), m_searchEdit);
    clearFind->setContext(Qt::WidgetShortcut);
    connect(clearFind, &QShortcut::activated, m_searchEdit, &QLineEdit::clear);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::fillModel);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() {
        if (m_entries.size() < 500) fillModel();
        else m_searchTimer->start(180);
    });
    locationBar->addWidget(m_upButton);
    locationBar->addWidget(m_locationLabel, 1);
    locationBar->addWidget(m_previewButton);
    locationBar->addWidget(m_searchEdit, 1);
    archiveLayout->addLayout(locationBar);
    m_previewStack = new QStackedWidget;
    m_previewHint = new QLabel("选择文件可预览文本或图片");
    m_previewHint->setAlignment(Qt::AlignCenter);
    m_previewHint->setWordWrap(true);
    m_previewHint->setProperty("i18n", "选择文件可预览文本或图片");
    m_previewText = new QPlainTextEdit;
    m_previewText->setReadOnly(true);
    m_previewImage = new QLabel;
    m_previewImage->setAlignment(Qt::AlignCenter);
    m_previewStack->addWidget(m_previewHint);
    m_previewStack->addWidget(m_previewText);
    m_previewStack->addWidget(m_previewImage);
    m_previewStack->setVisible(m_previewButton->isChecked());
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::startPreview);
    connect(m_previewButton, &QToolButton::toggled, this, [this](bool visible) {
        m_previewStack->setVisible(visible);
        QSettings().setValue(QStringLiteral("ui/previewVisible"), visible);
        updatePreview();
    });
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
    connect(table, &QTableView::doubleClicked, this, [this](const QModelIndex&) { openSelectedHere(); });
    auto* goUp = new QAction(this);
    goUp->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    connect(goUp, &QAction::triggered, this, &MainWindow::navigateUp);
    addAction(goUp);

    auto* empty = new DropLabel;
    empty->setObjectName("homeSurface");
    empty->setStyleSheet("QLabel#homeSurface { background: #f5f7fa; }");
    empty->setAcceptDrops(true);
    empty->onDrop = [this](const QList<QUrl>& urls) { handleDropped(urls); };
    empty->onDragActive = [this](bool active) { showDropHint(active); };
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setSizeConstraint(QLayout::SetMinimumSize);
    emptyLayout->setContentsMargins(28, 24, 28, 20);
    emptyLayout->setSpacing(20);
    emptyLayout->addStretch(1);
    auto* hero = new QFrame(empty);
    hero->setObjectName("heroCard");
    hero->setMaximumWidth(820);
    hero->setMinimumHeight(230);
    hero->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(30, 24, 20, 24);
    heroLayout->setSpacing(12);
    auto* heroCopy = new QVBoxLayout;
    heroCopy->setSpacing(10);
    auto* eyebrow = new QLabel("青匣 · ZIP / 7Z / RAR", hero);
    eyebrow->setObjectName("heroEyebrow");
    eyebrow->setProperty("i18n", "青匣 · ZIP / 7Z / RAR");
    auto* emptyTitle = new QLabel("文件收好，随时打开", hero);
    emptyTitle->setObjectName("emptyTitle");
    emptyTitle->setProperty("i18n", "文件收好，随时打开");
    emptyTitle->setWordWrap(true);
    emptyTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* emptyBody = new QLabel("拖入压缩包即可查看内容；拖入文件或文件夹即可创建压缩包。", hero);
    emptyBody->setObjectName("emptyBody");
    emptyBody->setProperty("i18n", "拖入压缩包即可查看内容；拖入文件或文件夹即可创建压缩包。");
    emptyBody->setWordWrap(true);
    emptyBody->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* emptyButtons = new QHBoxLayout;
    emptyButtons->setSpacing(10);
    auto* emptyOpen = new QPushButton("打开压缩包", hero);
    emptyOpen->setObjectName("homeOpen");
    emptyOpen->setIcon(toolGlyph("打开"));
    emptyOpen->setToolTip("Ctrl+O");
    emptyOpen->setProperty("i18n", "打开压缩包");
    auto* emptyNew = new QPushButton("新建压缩", hero);
    emptyNew->setObjectName("homeNew");
    emptyNew->setIcon(toolGlyph("新建压缩", Qt::white));
    emptyNew->setToolTip("Ctrl+N");
    emptyNew->setProperty("i18n", "新建压缩");
    emptyButtons->addWidget(emptyOpen);
    emptyButtons->addWidget(emptyNew);
    emptyButtons->addStretch();
    heroCopy->addStretch();
    heroCopy->addWidget(eyebrow);
    heroCopy->addWidget(emptyTitle);
    heroCopy->addWidget(emptyBody);
    heroCopy->addSpacing(6);
    heroCopy->addLayout(emptyButtons);
    heroCopy->addStretch();
    heroLayout->addLayout(heroCopy, 1);
    auto* illustration = new EmptyIllustration(hero);
    heroLayout->addWidget(illustration);
    emptyLayout->addWidget(hero, 0, Qt::AlignHCenter);
    m_recentSection = new QWidget(empty);
    m_recentSection->setMaximumWidth(820);
    m_recentSection->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* recentLayout = new QVBoxLayout(m_recentSection);
    recentLayout->setContentsMargins(2, 0, 2, 0);
    recentLayout->setSpacing(8);
    auto* recentHeading = new QHBoxLayout;
    auto* recentTitle = new QLabel("最近打开", m_recentSection);
    recentTitle->setObjectName("recentTitle");
    recentTitle->setProperty("i18n", "最近打开");
    auto* clearRecent = new QPushButton("清空记录", m_recentSection);
    clearRecent->setObjectName("quietButton");
    clearRecent->setProperty("i18n", "清空记录");
    recentHeading->addWidget(recentTitle);
    recentHeading->addStretch();
    recentHeading->addWidget(clearRecent);
    recentLayout->addLayout(recentHeading);
    m_recentList = new QListWidget(m_recentSection);
    m_recentList->setItemDelegate(new RecentArchiveDelegate(m_recentList));
    m_recentList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_recentList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_recentList->setObjectName("recentArchives");
    m_recentList->setFixedHeight(40);
    recentLayout->addWidget(m_recentList);
    emptyLayout->addWidget(m_recentSection, 0, Qt::AlignHCenter);
    auto* homeHint = new QLabel("支持 ZIP / 7z / RAR 浏览与解压 · ZIP / 7z 创建", empty);
    homeHint->setObjectName("homeHint");
    homeHint->setProperty("i18n", "支持 ZIP / 7z / RAR 浏览与解压 · ZIP / 7z 创建");
    homeHint->setAlignment(Qt::AlignCenter);
    homeHint->setWordWrap(true);
    emptyLayout->addWidget(homeHint);
    emptyLayout->addStretch(1);
    auto* homeScroll = new HomeScrollArea;
    homeScroll->onWidthChanged = [hero, illustration, homeHint, emptyLayout, this](int width) {
        const int contentWidth = std::max(0, std::min(820, width - 56));
        hero->setFixedWidth(contentWidth);
        illustration->setVisible(width >= 820);
        hero->setMinimumHeight(width < 820 ? 190 : 230);
        homeHint->setVisible(width >= 820);
        emptyLayout->setSpacing(width < 820 ? 12 : 20);
        emptyLayout->setContentsMargins(28, width < 820 ? 18 : 24, 28, width < 820 ? 12 : 20);
        m_recentSection->setFixedWidth(contentWidth);
    };
    connect(m_recentList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        openArchive(item->data(Qt::UserRole).toString());
    });
    auto* openRecentShortcut = new QShortcut(QKeySequence(Qt::Key_Return), m_recentList);
    connect(openRecentShortcut, &QShortcut::activated, this, [this]() {
        if (QListWidgetItem* item = m_recentList->currentItem()) {
            openArchive(item->data(Qt::UserRole).toString());
        }
    });
    connect(clearRecent, &QPushButton::clicked, this, [this]() {
        QSettings().remove(QStringLiteral("ui/recentArchives"));
        refreshRecentArchives();
    });
    refreshRecentArchives();
    connect(emptyOpen, &QPushButton::clicked, this, &MainWindow::chooseAndOpen);
    connect(emptyNew, &QPushButton::clicked, this, [this]() { createArchive({}); });
    homeScroll->setWidgetResizable(true);
    homeScroll->setFrameShape(QFrame::NoFrame);
    homeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    homeScroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");
    homeScroll->setWidget(empty);
    m_emptyPage = homeScroll;

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_emptyPage);
    m_stack->addWidget(m_archivePage);
    m_dropHint = new QLabel(m_stack);
    m_dropHint->setObjectName("dropHint");
    m_dropHint->setAlignment(Qt::AlignCenter);
    m_dropHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_dropHint->hide();
    m_stack->installEventFilter(this);
    setCentralWidget(m_stack);
    statusBar()->showMessage(zipdesk::translate("拖入压缩包可打开，拖入文件或文件夹可新建压缩"));
    installItemShortcuts();
    applyLanguage();
    updateActions();
}

void MainWindow::showDropHint(bool visible) {
    if (m_dropHint == nullptr || m_stack == nullptr) {
        return;
    }
    if (!visible || m_busy) {
        m_dropHint->hide();
        return;
    }
    const QString text = m_path.empty() ? QStringLiteral("松开以打开压缩包或新建压缩")
                                        : QStringLiteral("松开以追加到当前压缩包");
    m_dropHint->setText(zipdesk::translate(text));
    m_dropHint->setGeometry(m_stack->rect().adjusted(12, 8, -12, -8));
    m_dropHint->raise();
    m_dropHint->show();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_stack && event->type() == QEvent::Resize && m_dropHint != nullptr && m_dropHint->isVisible()) {
        m_dropHint->setGeometry(m_stack->rect().adjusted(12, 8, -12, -8));
    }
    return QMainWindow::eventFilter(watched, event);
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
    QSettings().setValue(QStringLiteral("ui/windowGeometry"), saveGeometry());
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
    case zipdesk::LaunchAction::SmartExtract:
        smartExtract(QString::fromStdWString(m_launch.paths.front()));
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
    for (QAction* action : m_toolbar->actions()) {
        if (m_toolbar->widgetForAction(action) == widget) {
            action->setVisible(visible);
            widget->setVisible(visible);
            return;
        }
    }
    widget->setVisible(visible);
}

void MainWindow::updateActions() {
    const bool open = !m_path.empty();
    const bool editable = open && zipdesk::formatFromPath(m_path) != zipdesk::ArchiveFormat::Rar;
    const bool hasSelection = open && !selectedIndices().empty();
    const QString extractStyle = open ? QStringLiteral("primaryButton") : QStringLiteral("toolButton");
    if (m_extractButton->objectName() != extractStyle) {
        m_extractButton->setObjectName(extractStyle);
        m_extractButton->setIcon(toolGlyph("解压", open ? QColor("#ffffff") : QColor("#3a6762")));
        style()->unpolish(m_extractButton);
        style()->polish(m_extractButton);
    }
    setBarItemVisible(m_openButton, true);
    setBarItemVisible(m_newButton, !open);
    setBarItemVisible(m_homeButton, open);
    setBarItemVisible(m_appendButton, open);
    setBarItemVisible(m_extractButton, true);
    setBarItemVisible(m_moreButton, open);
    setBarItemVisible(m_previewButton, open);
    m_homeButton->setEnabled(open && !m_busy);
    m_appendButton->setEnabled(editable && !m_busy);
    m_moreTestAction->setEnabled(open && !m_busy);
    m_moreRepairAction->setEnabled(open && !m_busy);
    m_moreCopyAction->setEnabled(hasSelection && !m_busy);
    m_moreMoveAction->setEnabled(hasSelection && editable && !m_busy);
    m_moreDeleteAction->setEnabled(hasSelection && editable && !m_busy);
    m_moreInfoAction->setEnabled(open && !m_busy);
    m_previewButton->setEnabled(open && !m_busy);
    m_extractButton->setEnabled(!m_busy);
    if (m_extractSelectedAction != nullptr) {
        m_extractSelectedAction->setVisible(open);
        m_extractSelectedAction->setEnabled(hasSelection && !m_busy);
    }
    if (m_extractSelectedFolderAction != nullptr) {
        m_extractSelectedFolderAction->setVisible(open);
        m_extractSelectedFolderAction->setEnabled(hasSelection && !m_busy);
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
    if (busy) {
        ++m_previewGeneration;
        m_previewTimer->stop();
        if (m_previewCancel) m_previewCancel->store(true);
    } else if (m_previewButton->isChecked()) {
        updatePreview();
    }
    updateActions();
}

void MainWindow::showArchive(zipdesk::ArchiveInfo info) {
    const bool differentArchive = m_path != info.path;
    m_path = info.path;
    if (!info.password.empty()) {
        m_password = info.password;
    }
    m_encrypted = info.encrypted || !m_password.empty();
    m_entries = std::move(info.entries);
    m_comment = info.comment;
    if (differentArchive) {
        m_prefix.clear();
        m_searchEdit->clear();
    } else {
        while (!m_prefix.empty() && std::none_of(m_entries.begin(), m_entries.end(), [this](const auto& entry) {
            return zipdesk::pathInside(m_prefix, entry.path);
        })) {
            const auto slash = m_prefix.find_last_of(L'/');
            m_prefix = slash == std::wstring::npos ? std::wstring{} : m_prefix.substr(0, slash);
        }
    }
    const QString openedPath = QString::fromStdWString(m_path);
    QStringList recent = QSettings().value(QStringLiteral("ui/recentArchives")).toStringList();
    recent.removeAll(openedPath);
    recent.prepend(openedPath);
    while (recent.size() > 5) {
        recent.removeLast();
    }
    QSettings().setValue(QStringLiteral("ui/recentArchives"), recent);
    refreshRecentArchives();
    fillModel();
    m_stack->setCurrentWidget(m_archivePage);
    const QString name = QFileInfo(QString::fromStdWString(m_path)).fileName();
    setWindowTitle(name + (zipdesk::uiLanguage() == zipdesk::UiLanguage::English ? " - QingXia" : " - 青匣"));
    updateActions();
}

void MainWindow::refreshRecentArchives() {
    if (m_recentList == nullptr) {
        return;
    }
    m_recentList->clear();
    const QStringList recent = QSettings().value(QStringLiteral("ui/recentArchives")).toStringList();
    for (const QString& path : recent) {
        if (path.isEmpty()) {
            continue;
        }
        auto* item = new QListWidgetItem(QFileInfo(path).fileName(), m_recentList);
        item->setIcon(archiveItemGlyph(false));
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
        item->setToolTip(QDir::toNativeSeparators(path));
    }
    m_recentList->setFixedHeight(std::min(m_recentList->count(), 2) * 62 + 10);
    m_recentSection->setVisible(m_recentList->count() > 0);
}

void MainWindow::fillModel() {
    m_searchTimer->stop();
    updateLocationBar();
    m_table->setSortingEnabled(false);
    const QString query = m_searchEdit->text().trimmed();
    std::vector<ArchiveListModel::Row> rows;
    rows.reserve(m_entries.size() + (!m_prefix.empty() && query.isEmpty() ? 1 : 0));
    if (!m_prefix.empty() && query.isEmpty()) {
        ArchiveListModel::Row up;
        up.name = "..";
        up.index = 0xFFFFFFFFu;
        up.directory = true;
        up.parent = true;
        rows.push_back(std::move(up));
    }
    const auto normalizedPath = [](const zipdesk::ArchiveEntry& entry) {
        QString path = QString::fromStdWString(entry.path).replace('\\', '/');
        while (path.endsWith('/')) path.chop(1);
        return path;
    };
    QHash<QString, const zipdesk::ArchiveEntry*> directoryEntries;
    if (query.isEmpty()) {
        for (const auto& entry : m_entries) {
            if (entry.isDir) directoryEntries.insert(normalizedPath(entry), &entry);
        }
    }
    const QString prefix = QString::fromStdWString(m_prefix);
    const QString prefixWithSlash = prefix.isEmpty() ? QString() : prefix + '/';
    QSet<QString> visibleDirectories;
    const auto addRow = [&rows](const zipdesk::ArchiveEntry* entry, const QString& display,
                               const QString& path, bool directory) {
        ArchiveListModel::Row row;
        row.name = display;
        row.path = path;
        row.directory = directory;
        row.virtualDirectory = entry == nullptr;
        if (entry) {
            row.index = entry->index;
            row.size = entry->size;
            row.packed = entry->packSize;
            if (entry->hasModified) row.modified = QString::fromStdWString(entry->modified);
        }
        rows.push_back(std::move(row));
    };
    for (const zipdesk::ArchiveEntry& entry : m_entries) {
        const QString path = normalizedPath(entry);
        if (path.isEmpty()) continue;
        if (!query.isEmpty()) {
            if (path.contains(query, Qt::CaseInsensitive)) {
                addRow(&entry, QString(path).replace('/', '\\'), path, entry.isDir);
            }
            continue;
        }
        if (!prefixWithSlash.isEmpty() && !path.startsWith(prefixWithSlash)) continue;
        const QString relative = path.mid(prefixWithSlash.size());
        if (relative.isEmpty()) continue;
        const int slash = relative.indexOf('/');
        if (slash >= 0 || entry.isDir) {
            const QString childName = slash >= 0 ? relative.left(slash) : relative;
            const QString childPath = prefixWithSlash + childName;
            if (visibleDirectories.contains(childPath)) continue;
            visibleDirectories.insert(childPath);
            addRow(directoryEntries.value(childPath, nullptr), childName, childPath, true);
        } else {
            addRow(&entry, relative, path, false);
        }
    }
    m_model->setRows(std::move(rows));
    m_table->setSortingEnabled(true);
    m_fileStack->setCurrentIndex(m_model->rowCount() == 0 ? 1 : 0);
    m_emptyResultTitle->setText(zipdesk::translate(query.isEmpty() ? "压缩包里还没有文件" : "没有找到匹配的文件"));
    m_emptyResultBody->setText(zipdesk::translate(query.isEmpty()
        ? "添加文件，或将文件拖入窗口。" : "试试更短的关键词，或清除搜索查看全部内容。"));
    m_clearSearchButton->setVisible(!query.isEmpty());
    quint64 totalSize = 0;
    for (const auto& entry : m_entries) if (!entry.isDir) totalSize += entry.size;
    const QString format = QFileInfo(QString::fromStdWString(m_path)).suffix().toUpper();
    m_archiveSummary->setText(QString("%1  ·  %2  ·  %3")
        .arg(format, zipdesk::translate("%1 项").arg(m_entries.size()), humanSize(totalSize)));
    if (!m_path.empty()) {
        statusBar()->showMessage(query.isEmpty()
            ? zipdesk::translate("当前显示 %1 项 · 压缩包共 %2 项 · %3")
                  .arg(m_model->rowCount() - (m_prefix.empty() ? 0 : 1))
                  .arg(m_entries.size())
                  .arg(zipdesk::translate("拖入文件可追加到压缩包"))
            : zipdesk::translate("找到 %1 项").arg(m_model->rowCount()));
    }
}

void MainWindow::updateLocationBar() {
    const QString name = QFileInfo(QString::fromStdWString(m_path)).fileName();
    const QString folder = QString::fromStdWString(m_prefix).replace('/', "  ›  ");
    m_locationLabel->setText(folder.isEmpty() ? name : name + "  ›  " + folder);
    m_locationLabel->setToolTip(QDir::toNativeSeparators(QString::fromStdWString(m_path))
        + (m_prefix.empty() ? QString() : "\n" + QString::fromStdWString(m_prefix)));
    m_upButton->setEnabled(!m_prefix.empty());
}

void MainWindow::navigateUp() {
    if (m_prefix.empty()) return;
    const auto slash = m_prefix.find_last_of(L'/');
    m_prefix = slash == std::wstring::npos ? std::wstring{} : m_prefix.substr(0, slash);
    m_searchEdit->clear();
    fillModel();
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
    m_activeOperation = title;
    m_progress->reset(title);
    m_progress->setWindowModality(Qt::ApplicationModal);
    if (isShellExtract()) {
        setAttribute(Qt::WA_DontShowOnScreen, true);
        hide();
        m_progress->setParent(nullptr);
        m_progress->setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    }
    m_progress->show();
    m_progress->raise();
    setBusy(true);
    m_job->start(std::move(task));
}

void MainWindow::onSucceeded() {
    m_activeOperation.clear();
    m_extractOutputDir.clear();
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
    if (isShellAction()) {
        QCoreApplication::quit();
    }
}

void MainWindow::onFailed(const QString& message) {
    const bool moving = m_activeOperation == "正在移动";
    const QString extractDir = m_extractOutputDir;
    m_extractOutputDir.clear();
    m_activeOperation.clear();
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
    if (extractDir.isEmpty()) {
        QMessageBox::warning(isShellExtract() ? nullptr : this, "青匣", message);
    } else {
        showExtractIssue(moving ? "移动未完成" : "解压未完成", message + "\n\n目标目录可能已有部分文件，请检查后重试。",
                         extractDir, true);
    }
    if (isShellAction()) {
        QCoreApplication::quit();
    }
}

void MainWindow::onCancelled() {
    const bool moving = m_activeOperation == "正在移动";
    const QString extractDir = m_extractOutputDir;
    m_extractOutputDir.clear();
    m_activeOperation.clear();
    m_progress->hide();
    setBusy(false);
    m_onSuccess = nullptr;
    if (m_inDrag) {
        m_dragDone = true;
        m_dragOk = false;
        return;
    }
    if (m_closeAfterJob) {
        close();
    } else if (extractDir.isEmpty()) {
        statusBar()->showMessage("已取消");
    } else {
        showExtractIssue(moving ? "移动已取消" : "解压已取消", "目标目录可能已有部分文件，请检查。", extractDir, false);
    }
    if (isShellAction()) {
        QCoreApplication::quit();
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

std::optional<zipdesk::OverwritePolicy> MainWindow::askOverwrite(bool allowSkip) {
    QMessageBox box(m_progress->isVisible() ? static_cast<QWidget*>(m_progress) : static_cast<QWidget*>(this));
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("同名文件");
    box.setText(allowSkip ? "目标位置已有同名文件。这一批要如何处理？" : "目标位置已有同名文件。移动前必须完整解压，是否覆盖目标文件？");
    auto* overwrite = box.addButton("全部覆盖", QMessageBox::AcceptRole);
    auto* skip = allowSkip ? box.addButton("全部跳过", QMessageBox::ActionRole) : nullptr;
    box.addButton("取消", QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == overwrite) {
        return zipdesk::OverwritePolicy::Overwrite;
    }
    if (allowSkip && box.clickedButton() == skip) {
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
    QSet<std::uint32_t> seen;
    for (const QModelIndex& index : rows) {
        if (index.data(Qt::UserRole + 2).toBool()) {
            continue;
        }
        if (index.data(Qt::UserRole + 4).toBool()) {
            const std::wstring folder = index.data(Qt::UserRole + 3).toString().toStdWString();
            for (const auto& entry : m_entries) {
                if (zipdesk::pathInside(folder, entry.path) && !seen.contains(entry.index)) {
                    seen.insert(entry.index);
                    indices.push_back(entry.index);
                }
            }
        } else {
            const auto itemIndex = index.data(Qt::UserRole).toUInt();
            if (!seen.contains(itemIndex)) {
                seen.insert(itemIndex);
                indices.push_back(itemIndex);
            }
        }
    }
    return indices;
}

QStringList MainWindow::selectedTopLevelPaths() const {
    struct SelectedPath { QString path; bool directory; };
    QList<SelectedPath> selected;
    for (const QModelIndex& index : m_table->selectionModel()->selectedRows()) {
        if (index.data(Qt::UserRole + 2).toBool()) continue;
        const QString path = index.data(Qt::UserRole + 3).toString();
        if (!path.isEmpty()) selected.push_back({path, index.data(Qt::UserRole + 5).toBool()});
    }
    QStringList result;
    for (const auto& candidate : selected) {
        bool covered = false;
        for (const auto& other : selected) {
            if (other.directory && other.path != candidate.path &&
                candidate.path.startsWith(other.path + '/')) {
                covered = true;
                break;
            }
        }
        if (!covered && !result.contains(candidate.path)) result.push_back(candidate.path);
    }
    return result;
}

void MainWindow::chooseAndOpen() {
    const QString path = QFileDialog::getOpenFileName(this, "打开压缩包", QString(), "压缩包 (*.zip *.7z *.rar)");
    if (!path.isEmpty()) {
        openArchive(path);
    }
}

void MainWindow::openArchive(const QString& path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, "青匣", "找不到压缩包。");
        return;
    }
    if (!zipdesk::isArchivePath(path.toStdWString())) {
        QMessageBox::warning(this, "青匣", "只支持 zip、7z 和 rar。");
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
        const auto answer = QMessageBox::question(this, "青匣", output + " 已存在，要覆盖吗？");
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    begin(
        "正在压缩",
        [this, request](const zipdesk::OperationHooks& hooks) { m_service.compress(request, hooks); },
        [this, output]() { finishCompress(output); });
}

bool MainWindow::isShellAction() const {
    return m_launch.action == zipdesk::LaunchAction::CompressZip ||
           m_launch.action == zipdesk::LaunchAction::Compress7z ||
           m_launch.action == zipdesk::LaunchAction::ExtractHere ||
           m_launch.action == zipdesk::LaunchAction::ExtractToFolder ||
           m_launch.action == zipdesk::LaunchAction::SmartExtract;
}

bool MainWindow::isShellExtract() const {
    return m_launch.action == zipdesk::LaunchAction::ExtractHere ||
           m_launch.action == zipdesk::LaunchAction::ExtractToFolder ||
           m_launch.action == zipdesk::LaunchAction::SmartExtract;
}

void MainWindow::finishCompress(const QString& output) {
    const QString native = QDir::toNativeSeparators(QFileInfo(output).absoluteFilePath());
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + native});
    statusBar()->showMessage("已生成 " + native);
}

void MainWindow::finishExtract(const QString& directory, bool skippedExisting) {
    statusBar()->showMessage("解压操作已完成：" + directory);
    QMessageBox box(isShellExtract() ? nullptr : this);
    box.setWindowTitle("解压完成");
    box.setText("解压操作已完成。" +
                (skippedExisting ? QString("\n同名文件已按“全部跳过”规则处理。") : QString()) +
                "\n目标目录：\n" + QDir::toNativeSeparators(directory));
    auto* open = box.addButton("打开目标文件夹", QMessageBox::ActionRole);
    box.addButton("完成", QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() == open) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    }
}
void MainWindow::showExtractIssue(const QString& title, const QString& message,
                                  const QString& directory, bool failed) {
    QMessageBox box(isShellExtract() ? nullptr : this);
    box.setIcon(failed ? QMessageBox::Warning : QMessageBox::Information);
    box.setWindowTitle(title);
    box.setText(message + "\n\n目标目录：\n" + QDir::toNativeSeparators(directory));
    QPushButton* open = nullptr;
    if (QDir(directory).exists()) {
        open = box.addButton("打开目标文件夹", QMessageBox::ActionRole);
    }
    box.addButton("关闭", QMessageBox::AcceptRole);
    box.exec();
    if (open != nullptr && box.clickedButton() == open) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
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
    m_searchEdit->clear();
    m_comment.clear();
    m_encrypted = false;
    m_model->setRows({});
    m_stack->setCurrentWidget(m_emptyPage);
    setWindowTitle(zipdesk::uiLanguage() == zipdesk::UiLanguage::English ? "QingXia" : "青匣");
    statusBar()->showMessage(zipdesk::translate("拖入压缩包可打开，拖入文件或文件夹可新建压缩"));
    updateActions();
}

void MainWindow::showSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle("设置");
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(12);
    layout->addWidget(new QLabel("界面语言", &dialog));
    auto* chinese = new QRadioButton("简体中文", &dialog);
    auto* english = new QRadioButton("English", &dialog);
    QButtonGroup languageGroup(&dialog);
    languageGroup.addButton(chinese);
    languageGroup.addButton(english);
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
    dialog.resize(520, 280);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const bool integrated = zipdesk::shellIntegrationInstalled(QCoreApplication::applicationFilePath().toStdWString());
    if (shell->isChecked() != integrated) {
        toggleShellIntegration();
    }
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
        } else if (auto* button = qobject_cast<QPushButton*>(widget)) {
            button->setText(zipdesk::translate(key));
        }
    };
    for (QWidget* widget : findChildren<QWidget*>()) {
        refresh(widget);
    }
    for (QAction* action : findChildren<QAction*>()) {
        const QString key = action->property("i18n").toString();
        if (!key.isEmpty()) {
            action->setText(zipdesk::translate(key));
        }
    }
    const QString appName = zipdesk::uiLanguage() == zipdesk::UiLanguage::English ? "QingXia" : "青匣";
    setWindowTitle(m_path.empty() ? appName : QFileInfo(QString::fromStdWString(m_path)).fileName() + " - " + appName);
    m_upButton->setToolTip(zipdesk::translate("上一级") + " (Alt+↑)");
    m_searchEdit->setPlaceholderText(zipdesk::uiLanguage() == zipdesk::UiLanguage::English
        ? "Search archive · Ctrl+F" : "搜索整个压缩包 · Ctrl+F");
    m_searchEdit->setAccessibleName(zipdesk::translate("搜索整个压缩包"));
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
    if (!m_path.empty()) fillModel();
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
                QMessageBox::warning(this, "青匣", QString::fromStdWString(error.message()));
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
        const auto skipped = std::make_shared<std::atomic_bool>(false);
        m_extractOutputDir = finishedDir;
        begin(
            "正在解压",
            [this, archive, outputDir, created, skipped](const zipdesk::OperationHooks& hooks) {
                const zipdesk::ArchiveInfo info = m_service.read(archive, L"", hooks);
                zipdesk::OverwritePolicy policy = zipdesk::OverwritePolicy::Overwrite;
                if (zipdesk::hasConflicts(info.entries, {}, outputDir)) {
                    std::optional<zipdesk::OverwritePolicy> choice;
                    QMetaObject::invokeMethod(
                        this, [&]() { choice = askOverwrite(); }, Qt::BlockingQueuedConnection);
                    if (!choice) {
                        if (created) {
                            std::error_code error;
                            std::filesystem::remove(outputDir, error);
                        }
                        throw zipdesk::ArchiveError(zipdesk::ErrorKind::Cancelled, L"已取消");
                    }
                    policy = *choice;
                    skipped->store(policy == zipdesk::OverwritePolicy::Skip);
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
            [this, finishedDir, skipped]() {
                finishExtract(finishedDir, skipped->load());
            });
        return;
    }
    if (m_entries.empty()) {
        QMessageBox::information(this, "青匣", "这个压缩包是空的。");
        return;
    }
    std::vector<std::uint32_t> indices;
    if (selectedOnly) {
        indices = selectedIndices();
        if (indices.empty()) {
            QMessageBox::information(this, "青匣", "请先选择要解压的项目。");
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
            QMessageBox::warning(this, "青匣", QString::fromStdWString(error.message()));
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
                std::filesystem::remove(outputDir, error);
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
    const bool skippedExisting = policy == zipdesk::OverwritePolicy::Skip;
    m_extractOutputDir = finishedDir;
    begin(
        "正在解压",
        [this, request](const zipdesk::OperationHooks& hooks) { m_service.extract(request, hooks); },
        [this, finishedDir, skippedExisting]() {
            finishExtract(finishedDir, skippedExisting);
        });
}

void MainWindow::smartExtract(const QString& path) {
    const std::wstring archive = QFileInfo(path).absoluteFilePath().toStdWString();
    const std::wstring password = archive == m_path ? m_password : L"";
    const auto result = std::make_shared<QString>(QFileInfo(path).absolutePath());
    const auto skipped = std::make_shared<std::atomic_bool>(false);
    m_extractOutputDir = *result;
    begin("正在解压",
        [this, archive, password, result, skipped](const zipdesk::OperationHooks& hooks) {
            const auto info = m_service.read(archive, password, hooks);
            const auto destination = zipdesk::smartExtractDestination(archive, info.entries);
            QMetaObject::invokeMethod(this, [&]() {
                *result = QString::fromStdWString(destination.resultDir);
                m_extractOutputDir = *result;
            }, Qt::BlockingQueuedConnection);
            const bool created = std::filesystem::create_directories(destination.outputDir);
            auto policy = zipdesk::OverwritePolicy::Overwrite;
            if (zipdesk::hasConflicts(info.entries, {}, destination.outputDir)) {
                std::optional<zipdesk::OverwritePolicy> choice;
                QMetaObject::invokeMethod(this, [&]() { choice = askOverwrite(); },
                                          Qt::BlockingQueuedConnection);
                if (!choice) {
                    if (created) {
                        std::error_code error;
                        std::filesystem::remove(destination.outputDir, error);
                    }
                    throw zipdesk::ArchiveError(zipdesk::ErrorKind::Cancelled, L"已取消");
                }
                policy = *choice;
                skipped->store(policy == zipdesk::OverwritePolicy::Skip);
            }
            zipdesk::ExtractRequest request;
            request.archivePath = archive;
            request.outputDir = destination.outputDir;
            request.password = info.password;
            request.overwrite = policy;
            request.removeOutputOnCancel = created;
            request.relativePaths = relativePathsOf(info.entries, {});
            m_service.extract(request, hooks);
        },
        [this, result, skipped]() { finishExtract(*result, skipped->load()); });
}
void MainWindow::shellExtract(bool namedFolder) {
    if (m_launch.paths.empty()) {
        QCoreApplication::quit();
        return;
    }
    const std::wstring archive = m_launch.paths.front();
    const std::filesystem::path archiveFile(archive);
    const std::filesystem::path destination = namedFolder
        ? archiveFile.parent_path() / archiveFile.stem() : archiveFile.parent_path();
    const QString finishedDir = QString::fromStdWString(destination.wstring());
    const auto skipped = std::make_shared<std::atomic_bool>(false);
    m_extractOutputDir = finishedDir;
    begin(
        "正在解压",
        [this, archive, namedFolder, skipped](const zipdesk::OperationHooks& hooks) {
            const zipdesk::ArchiveInfo info = m_service.read(archive, L"", hooks);
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
                        std::filesystem::remove(outputDir, error);
                    }
                    throw zipdesk::ArchiveError(zipdesk::ErrorKind::Cancelled, L"已取消");
                }
                policy = *choice;
                skipped->store(policy == zipdesk::OverwritePolicy::Skip);
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
        [this, finishedDir, skipped]() { finishExtract(finishedDir, skipped->load()); });
}

void MainWindow::shellCompress(zipdesk::ArchiveFormat format) {
    if (m_launch.paths.empty()) {
        QCoreApplication::quit();
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
        QMessageBox::warning(this, "青匣", "找不到要压缩的文件。");
        QCoreApplication::quit();
        return;
    }
    const QString extension = format == zipdesk::ArchiveFormat::Zip ? ".zip" : ".7z";
    const QString output = defaultArchivePath(sources, extension);
    if (QFileInfo::exists(output)) {
        const auto answer = QMessageBox::question(this, "青匣", output + " 已存在，要覆盖吗？");
        if (answer != QMessageBox::Yes) {
            QCoreApplication::quit();
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
        [this, output]() { finishCompress(output); });
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
        QMessageBox::warning(this, "青匣", message);
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
    for (const QString& relative : selectedTopLevelPaths()) {
        const QString path = QDir(tempDir).filePath(relative);
        if (QFileInfo::exists(path)) urls.push_back(QUrl::fromLocalFile(path));
    }
    return urls;
}
