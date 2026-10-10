#include "MainWindow.hpp"

#include "ArchiveJob.hpp"
#include "Lang.hpp"

#include <QLabel>
#include <QStackedWidget>

#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMimeData>
#include <QUuid>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QStatusBar>
#include <QTableView>
#include <QUrl>

#include <filesystem>

namespace {

QString tempDirectory(const QString& name) {
    const QString path = QDir(QDir::tempPath()).filePath("ZipDesk/" + name);
    QDir().mkpath(path);
    return path;
}

QString entryFile(const QString& root, const zipdesk::ArchiveEntry& entry) {
    return QDir(root).filePath(QString::fromStdWString(entry.path));
}

QString parentPathOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L'/');
    if (slash == std::wstring::npos) {
        return {};
    }
    return QString::fromStdWString(path.substr(0, slash));
}

void splitFile(const QString& source, const QString& directory, qint64 partSize) {
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        return;
    }
    const QString base = QFileInfo(source).fileName();
    int part = 1;
    while (!input.atEnd()) {
        QFile output(QDir(directory).filePath(QString("%1.%2").arg(base).arg(part, 3, 10, QChar('0'))));
        if (!output.open(QIODevice::WriteOnly)) {
            return;
        }
        output.write(input.read(partSize));
        ++part;
    }
}

}  // namespace

void MainWindow::installItemShortcuts() {
    const auto add = [this](const QKeySequence& sequence, void (MainWindow::*method)()) {
        auto* action = new QAction(this);
        action->setShortcut(sequence);
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, method);
        addAction(action);
        m_itemActions.push_back(action);
    };
    add(Qt::Key_Return, &MainWindow::openSelectedHere);
    add(Qt::Key_Enter, &MainWindow::openSelectedHere);
    auto* here = new QAction(this);
    here->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_PageDown));
    connect(here, &QAction::triggered, this, &MainWindow::openSelectedHere);
    addAction(here);
    m_itemActions.push_back(here);
    auto* fresh = new QAction(this);
    fresh->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return));
    connect(fresh, &QAction::triggered, this, [this]() { openSelected(true); });
    addAction(fresh);
    m_itemActions.push_back(fresh);
    add(Qt::Key_F3, &MainWindow::viewSelected);
    add(Qt::Key_F4, &MainWindow::editSelected);
    add(Qt::Key_F2, &MainWindow::renameSelected);
    add(Qt::Key_F5, &MainWindow::copySelectedTo);
    auto* copyClip = new QAction(m_table);
    copyClip->setShortcut(QKeySequence::Copy);
    copyClip->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(copyClip, &QAction::triggered, this, &MainWindow::copySelectedToClipboard);
    m_table->addAction(copyClip);
    m_itemActions.push_back(copyClip);
    add(Qt::Key_F6, &MainWindow::moveSelectedTo);
    add(QKeySequence::Delete, &MainWindow::deleteSelected);
    auto* props = new QAction(this);
    props->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
    connect(props, &QAction::triggered, this, &MainWindow::showProperties);
    addAction(props);
    m_itemActions.push_back(props);
    auto* comment = new QAction(this);
    comment->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z));
    connect(comment, &QAction::triggered, this, &MainWindow::showComment);
    addAction(comment);
    m_itemActions.push_back(comment);
    add(Qt::Key_F7, &MainWindow::createFolder);
    auto* file = new QAction(this);
    file->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
    connect(file, &QAction::triggered, this, &MainWindow::createFile);
    addAction(file);
    m_itemActions.push_back(file);
}

void MainWindow::showItemMenu(const QPoint& position) {
    const QModelIndex index = m_table->indexAt(position);
    if (!index.isValid() || m_path.empty()) {
        return;
    }
    m_table->selectRow(index.row());
    QMenu menu(this);
    const auto label = [](const char* zh) { return zipdesk::translate(QString::fromUtf8(zh)); };
    menu.addAction(label("打开"), Qt::Key_Return, this, [this]() { openSelected(false); });
    menu.addAction(label("当前窗口打开"), QKeySequence(Qt::CTRL | Qt::Key_PageDown), this, [this]() { openSelectedHere(); });
    menu.addAction(label("新建窗口打开"), QKeySequence(Qt::SHIFT | Qt::Key_Return), this, [this]() { openSelected(true); });
    menu.addAction(label("查看"), Qt::Key_F3, this, [this]() { viewSelected(); });
    menu.addAction(label("编辑"), Qt::Key_F4, this, [this]() { editSelected(); });
    menu.addSeparator();
    menu.addAction("重命名", Qt::Key_F2, this, [this]() { renameSelected(); });
    menu.addAction(label("复制"), QKeySequence::Copy, this, [this]() { copySelectedToClipboard(); });
    menu.addAction("复制到...", Qt::Key_F5, this, [this]() { copySelectedTo(); });
    menu.addAction("移动到...", Qt::Key_F6, this, [this]() { moveSelectedTo(); });
    menu.addAction("删除", QKeySequence::Delete, this, [this]() { deleteSelected(); });
    menu.addSeparator();
    menu.addAction("分割文件...", this, [this]() { splitSelected(); });
    menu.addAction("合并文件...", this, [this]() { mergeFiles(); });
    menu.addSeparator();
    menu.addAction("属性", QKeySequence(Qt::ALT | Qt::Key_Return), this, [this]() { showProperties(); });
    menu.addAction("注释", QKeySequence(Qt::CTRL | Qt::Key_Z), this, [this]() { showComment(); });
    auto* crc = menu.addMenu("CRC");
    crc->addAction("CRC-32", this, [this]() { showCrc(); });
    menu.addAction("新建文件夹", Qt::Key_F7, this, [this]() { createFolder(); });
    menu.addAction("新建文件", QKeySequence(Qt::CTRL | Qt::Key_N), this, [this]() { createFile(); });
    menu.addAction("链接", this, [this]() { showLink(); });
    menu.addAction("交替数据流", this, [this]() { showStreams(); });
    menu.exec(m_table->viewport()->mapToGlobal(position));
}

std::optional<zipdesk::ArchiveEntry> MainWindow::selectedEntry() const {
    const auto entries = selectedEntries();
    if (entries.size() != 1) {
        return std::nullopt;
    }
    return entries.front();
}

std::vector<zipdesk::ArchiveEntry> MainWindow::selectedEntries() const {
    std::vector<zipdesk::ArchiveEntry> result;
    for (const std::uint32_t index : selectedIndices()) {
        for (const zipdesk::ArchiveEntry& entry : m_entries) {
            if (entry.index == index) {
                result.push_back(entry);
                break;
            }
        }
    }
    return result;
}

void MainWindow::reloadArchive() {
    const QString path = QString::fromStdWString(m_path);
    if (!path.isEmpty()) {
        openArchive(path);
    }
}

void MainWindow::extractEntries(const std::vector<zipdesk::ArchiveEntry>& entries,
                                const QString& directory,
                                const std::function<void()>& after) {
    if (entries.empty() || m_path.empty()) {
        return;
    }
    zipdesk::ExtractRequest request;
    request.archivePath = m_path;
    request.outputDir = directory.toStdWString();
    request.password = m_password;
    for (const zipdesk::ArchiveEntry& entry : entries) {
        request.indices.push_back(entry.index);
        request.relativePaths.push_back(entry.path);
    }
    const bool created = !QDir(directory).exists();
    QDir().mkpath(directory);
    request.removeOutputOnCancel = created;
    begin("正在解压", [this, request](const zipdesk::OperationHooks& hooks) { m_service.extract(request, hooks); }, after);
}

void MainWindow::openSelected(bool newWindow) {
    const auto entry = selectedEntry();
    if (!entry || entry->isDir) {
        if (newWindow && !m_path.empty()) {
            QProcess::startDetached(QCoreApplication::applicationFilePath(), {QString::fromStdWString(m_path)});
        }
        return;
    }
    const QString root = tempDirectory("open");
    const QString file = entryFile(root, *entry);
    extractEntries({*entry}, root, [this, entry = *entry, file, newWindow]() {
        if (newWindow && zipdesk::isArchivePath(file.toStdWString())) {
            QProcess::startDetached(QCoreApplication::applicationFilePath(), {file});
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
    });
}

void MainWindow::openSelectedHere() {
    const QModelIndex current = m_table->currentIndex();
    if (current.isValid() && current.data(Qt::UserRole + 2).toBool()) {
        const auto slash = m_prefix.find_last_of(L'/');
        m_prefix = slash == std::wstring::npos ? std::wstring{} : m_prefix.substr(0, slash);
        fillModel();
        return;
    }
    const auto entry = selectedEntry();
    if (!entry) {
        return;
    }
    if (entry->isDir) {
        m_prefix = entry->path;
        fillModel();
        statusBar()->showMessage(QString::fromStdWString(m_prefix));
        return;
    }
    openSelected(false);
}

void MainWindow::viewSelected() {
    const auto entry = selectedEntry();
    if (!entry || entry->isDir) {
        return;
    }
    const QString root = tempDirectory("view");
    const QString file = entryFile(root, *entry);
    extractEntries({*entry}, root, [file]() { QProcess::startDetached("notepad.exe", {file}); });
}

bool isTextFile(const QString& name) {
    const QString lower = name.toLower();
    static const QStringList suffixes = {".txt", ".md", ".json", ".xml", ".csv", ".log", ".ini", ".cpp", ".h", ".hpp", ".py", ".js", ".html", ".css"};
    for (const QString& suffix : suffixes) {
        if (lower.endsWith(suffix)) {
            return true;
        }
    }
    return false;
}

bool isImageFile(const QString& name) {
    const QString lower = name.toLower();
    return lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".bmp") ||
           lower.endsWith(".gif");
}

void MainWindow::updatePreview() {
    if (m_previewStack == nullptr || m_job->isRunning()) {
        return;
    }
    const auto entry = selectedEntry();
    if (!entry || entry->isDir || entry->size > 2 * 1024 * 1024) {
        m_previewStack->setCurrentIndex(0);
        return;
    }
    const QString name = QString::fromStdWString(entry->name);
    if (!isTextFile(name) && !isImageFile(name)) {
        m_previewStack->setCurrentIndex(0);
        return;
    }
    try {
        const std::vector<std::uint8_t> bytes = m_service.readItem(m_path, entry->index, m_password, {});
        const QByteArray data(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
        if (isImageFile(name)) {
            QPixmap pixmap;
            pixmap.loadFromData(data);
            m_previewImage->setPixmap(pixmap.scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_previewStack->setCurrentWidget(m_previewImage);
            return;
        }
        m_previewText->setPlainText(QString::fromUtf8(data));
        m_previewStack->setCurrentWidget(m_previewText);
    } catch (const zipdesk::ArchiveError&) {
        m_previewStack->setCurrentIndex(0);
    }
}

void MainWindow::repairArchive() {
    if (m_path.empty()) {
        return;
    }
    begin(
        "正在修复",
        [this](const zipdesk::OperationHooks& hooks) {
            const auto result = m_service.repair(m_path, m_password, hooks);
            QMetaObject::invokeMethod(this, [this, result]() {
                QMessageBox::information(this, "修复",
                                         QString("已恢复 %1 个文件，失败 %2 个。\n%3")
                                             .arg(result.recovered)
                                             .arg(result.failed)
                                             .arg(QString::fromStdWString(result.outputDir)));
            });
        },
        []() {});
}

void MainWindow::editSelected() {
    const auto entry = selectedEntry();
    if (!entry || entry->isDir) {
        return;
    }
    const QString name = QString::fromStdWString(entry->name);
    if (isTextFile(name)) {
        try {
            const std::vector<std::uint8_t> bytes = m_service.readItem(m_path, entry->index, m_password, {});
            QDialog dialog(this);
            dialog.setWindowTitle("编辑");
            auto* layout = new QVBoxLayout(&dialog);
            auto* editor = new QPlainTextEdit(QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())), &dialog);
            layout->addWidget(editor);
            auto* buttons = new QHBoxLayout;
            buttons->addStretch();
            auto* save = new QPushButton("保存", &dialog);
            save->setObjectName("primaryButton");
            auto* cancel = new QPushButton("关闭", &dialog);
            buttons->addWidget(save);
            buttons->addWidget(cancel);
            layout->addLayout(buttons);
            dialog.resize(640, 480);
            connect(save, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(1); });
            connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
            if (dialog.exec() != 1) {
                return;
            }
            const QString file = tempDirectory("edit") + "/" + name;
            QFile output(file);
            if (!output.open(QIODevice::WriteOnly)) {
                return;
            }
            output.write(editor->toPlainText().toUtf8());
            output.close();
            const auto index = entry->index;
            begin(
                "正在保存修改",
                [this, index, file](const zipdesk::OperationHooks& hooks) {
                    m_service.replaceItem(m_path, index, file.toStdWString(), m_password, hooks);
                },
                [this]() { reloadArchive(); });
            return;
        } catch (const zipdesk::ArchiveError& error) {
            QMessageBox::warning(this, "编辑", QString::fromStdWString(error.message()));
            return;
        }
    }
    const QString root = tempDirectory("edit");
    const QString file = entryFile(root, *entry);
    extractEntries({*entry}, root, [this, entry = *entry, file]() {
        auto* process = new QProcess(this);
        connect(process, &QProcess::finished, this, [this, process, entry, file]() {
            process->deleteLater();
            begin(
                "正在保存修改",
                [this, entry, file](const zipdesk::OperationHooks& hooks) {
                    m_service.replaceItem(m_path, entry.index, file.toStdWString(), m_password, hooks);
                },
                [this]() { reloadArchive(); });
        });
        process->start("notepad.exe", {file});
    });
}

void MainWindow::renameSelected() {
    const auto entry = selectedEntry();
    if (!entry) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "重命名", "新名称", QLineEdit::Normal,
                                               QString::fromStdWString(entry->name), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    const QString parent = parentPathOf(entry->path);
    const QString renamed = parent.isEmpty() ? name.trimmed() : parent + "/" + name.trimmed();
    const auto index = entry->index;
    begin(
        "正在重命名",
        [this, index, renamed](const zipdesk::OperationHooks& hooks) {
            m_service.renameItem(m_path, index, renamed.toStdWString(), m_password, hooks);
        },
        [this]() { reloadArchive(); });
}

void MainWindow::copySelectedToClipboard() {
    const auto entries = selectedEntries();
    if (entries.empty()) {
        statusBar()->showMessage("请先选择文件");
        return;
    }
    std::vector<std::uint32_t> selected;
    selected.reserve(entries.size());
    for (const zipdesk::ArchiveEntry& entry : entries) {
        selected.push_back(entry.index);
    }
    const std::vector<std::uint32_t> expanded = zipdesk::expandSelection(m_entries, selected);
    std::vector<zipdesk::ArchiveEntry> toExtract;
    for (const std::uint32_t index : expanded) {
        for (const zipdesk::ArchiveEntry& entry : m_entries) {
            if (entry.index == index) {
                toExtract.push_back(entry);
                break;
            }
        }
    }
    if (!m_clipboardDir.isEmpty()) {
        QDir(m_clipboardDir).removeRecursively();
    }
    const QString directory = tempDirectory("clip-" + QUuid::createUuid().toString(QUuid::Id128));
    m_clipboardDir = directory;
    extractEntries(toExtract, directory, [this, entries, directory]() {
        QList<QUrl> urls;
        for (const zipdesk::ArchiveEntry& entry : entries) {
            bool covered = false;
            for (const zipdesk::ArchiveEntry& other : entries) {
                if (!other.isDir || other.index == entry.index) {
                    continue;
                }
                const std::wstring prefix = !other.path.empty() && other.path.back() == L'/' ? other.path : other.path + L'/';
                if (entry.path.rfind(prefix, 0) == 0) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                urls.push_back(QUrl::fromLocalFile(entryFile(directory, entry)));
            }
        }
        auto* mime = new QMimeData;
        mime->setUrls(urls);
        QGuiApplication::clipboard()->setMimeData(mime);
        statusBar()->showMessage("已复制，可在其他文件夹按 Ctrl+V 粘贴");
    });
}

void MainWindow::copySelectedTo() {
    const auto entries = selectedEntries();
    if (entries.empty()) {
        QMessageBox::information(this, "日常解压", "请先选择文件。");
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(this, "复制到");
    if (directory.isEmpty()) {
        return;
    }
    extractEntries(entries, directory, [this, directory]() {
        statusBar()->showMessage("已复制到 " + directory);
    });
}

void MainWindow::moveSelectedTo() {
    const auto entries = selectedEntries();
    if (entries.empty()) {
        QMessageBox::information(this, "日常解压", "请先选择文件。");
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(this, "移动到");
    if (directory.isEmpty()) {
        return;
    }
    std::vector<std::uint32_t> indices;
    for (const auto& entry : entries) {
        indices.push_back(entry.index);
    }
    extractEntries(entries, directory, [this, indices, directory]() {
        begin(
            "正在从压缩包移除",
            [this, indices](const zipdesk::OperationHooks& hooks) {
                m_service.removeItems(m_path, indices, m_password, hooks);
            },
            [this, directory]() {
                statusBar()->showMessage("已移动到 " + directory);
                reloadArchive();
            });
    });
}

void MainWindow::deleteSelected() {
    const auto entries = selectedEntries();
    if (entries.empty()) {
        return;
    }
    if (QMessageBox::question(this, "删除", QString("删除选中的 %1 项？").arg(entries.size())) != QMessageBox::Yes) {
        return;
    }
    std::vector<std::uint32_t> indices;
    for (const auto& entry : entries) {
        indices.push_back(entry.index);
    }
    begin(
        "正在删除",
        [this, indices](const zipdesk::OperationHooks& hooks) {
            m_service.removeItems(m_path, indices, m_password, hooks);
        },
        [this]() { reloadArchive(); });
}

void MainWindow::testArchive() {
    if (m_path.empty()) {
        return;
    }
    begin("正在测试",
          [this](const zipdesk::OperationHooks& hooks) { m_service.test(m_path, m_password, hooks); },
          [this]() { QMessageBox::information(this, "测试", "压缩包测试通过。"); });
}

void MainWindow::showInfo() {
    if (const auto entry = selectedEntry()) {
        showProperties();
        return;
    }
    if (m_path.empty()) {
        return;
    }
    quint64 size = 0;
    for (const auto& entry : m_entries) {
        size += entry.size;
    }
    QMessageBox::information(this, "信息",
                             QString("压缩包：%1\n项目：%2\n原大小合计：%3\n加密：%4\n注释：%5")
                                 .arg(QString::fromStdWString(m_path))
                                 .arg(m_entries.size())
                                 .arg(size)
                                 .arg(m_encrypted ? "是" : "否")
                                 .arg(m_comment.empty() ? "无" : QString::fromStdWString(m_comment)));
}

void MainWindow::showProperties() {
    const auto entry = selectedEntry();
    if (!entry) {
        showInfo();
        return;
    }
    QMessageBox::information(
        this, "属性",
        QString("名称：%1\n路径：%2\n类型：%3\n原大小：%4\n压缩后大小：%5\n修改时间：%6\nCRC32：%7\n加密：%8\n链接：%9")
            .arg(QString::fromStdWString(entry->name))
            .arg(QString::fromStdWString(entry->path))
            .arg(entry->isDir ? "目录" : "文件")
            .arg(entry->size)
            .arg(entry->packSize)
            .arg(entry->hasModified ? QString::fromStdWString(entry->modified) : "无")
            .arg(entry->hasCrc ? QString("%1").arg(entry->crc, 8, 16, QChar('0')).toUpper() : "无")
            .arg(entry->encrypted ? "是" : "否")
            .arg(entry->isLink ? "是" : "否"));
}

void MainWindow::showCrc() {
    const auto entry = selectedEntry();
    if (!entry || !entry->hasCrc) {
        QMessageBox::information(this, "CRC", "当前项目没有 CRC。");
        return;
    }
    QMessageBox::information(this, "CRC-32",
                             QString("%1\nCRC32：%2")
                                 .arg(QString::fromStdWString(entry->path))
                                 .arg(QString("%1").arg(entry->crc, 8, 16, QChar('0')).toUpper()));
}

void MainWindow::showComment() {
    QMessageBox::information(this, "注释", m_comment.empty() ? "这个压缩包没有注释。" : QString::fromStdWString(m_comment));
}

void MainWindow::showLink() {
    const auto entry = selectedEntry();
    if (!entry) {
        return;
    }
    QMessageBox::information(this, "链接", entry->isLink ? "这是一个符号链接。" : "这不是链接。");
}

void MainWindow::showStreams() {
    const auto entry = selectedEntry();
    if (!entry) {
        return;
    }
    QStringList streams;
    const QString prefix = QString::fromStdWString(entry->path) + ":";
    for (const auto& item : m_entries) {
        const QString path = QString::fromStdWString(item.path);
        if (path.startsWith(prefix)) {
            streams << path;
        }
    }
    QMessageBox::information(this, "交替数据流", streams.isEmpty() ? "没有交替数据流。" : streams.join("\n"));
}

void MainWindow::createFolder() {
    if (m_path.empty()) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "新建文件夹", "文件夹名称", QLineEdit::Normal, "新建文件夹", &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    const QString folder = tempDirectory("new") + "/" + name.trimmed();
    QDir().mkpath(folder);
    appendPaths({folder});
}

void MainWindow::createFile() {
    if (m_path.empty()) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "新建文件", "文件名称", QLineEdit::Normal, "新建文件.txt", &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    const QString file = tempDirectory("new") + "/" + name.trimmed();
    QFile output(file);
    output.open(QIODevice::WriteOnly);
    appendPaths({file});
}

void MainWindow::splitSelected() {
    const auto entry = selectedEntry();
    if (!entry || entry->isDir) {
        QMessageBox::information(this, "分割文件", "请选择一个文件。");
        return;
    }
    bool ok = false;
    const int megabytes = QInputDialog::getInt(this, "分割文件", "每一卷大小（MB）", 10, 1, 4096, 1, &ok);
    if (!ok) {
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(this, "分卷保存到");
    if (directory.isEmpty()) {
        return;
    }
    const QString root = tempDirectory("split");
    const QString file = entryFile(root, *entry);
    extractEntries({*entry}, root, [this, file, directory, megabytes]() {
        splitFile(file, directory, static_cast<qint64>(megabytes) * 1024 * 1024);
        QMessageBox::information(this, "分割文件", "已分割到\n" + directory);
    });
}

void MainWindow::mergeFiles() {
    const QStringList parts = QFileDialog::getOpenFileNames(this, "按顺序选择要合并的文件");
    if (parts.size() < 2) {
        return;
    }
    const QString output = QFileDialog::getSaveFileName(this, "合并保存为");
    if (output.isEmpty()) {
        return;
    }
    QFile target(output);
    if (!target.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "合并文件", "无法写入目标文件。");
        return;
    }
    for (const QString& part : parts) {
        QFile input(part);
        if (!input.open(QIODevice::ReadOnly)) {
            continue;
        }
        target.write(input.readAll());
    }
    QMessageBox::information(this, "合并文件", "已合并为\n" + output);
}
