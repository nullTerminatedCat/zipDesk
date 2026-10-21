#include "MainWindow.hpp"

#include "ArchiveJob.hpp"
#include "Lang.hpp"

#include <QLabel>
#include <QLineEdit>
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
#include <QSaveFile>
#include <QTemporaryDir>
#include <QHBoxLayout>
#include <QHash>
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
#include <QToolButton>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <algorithm>
#include <memory>

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

bool writeFully(QIODevice& output, const QByteArray& bytes) {
    qsizetype written = 0;
    while (written < bytes.size()) {
        const qint64 count = output.write(bytes.constData() + written, bytes.size() - written);
        if (count <= 0) {
            return false;
        }
        written += static_cast<qsizetype>(count);
    }
    return true;
}

QString splitFile(const QString& source, const QString& directory, qint64 partSize) {
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        return "无法读取源文件：" + input.errorString();
    }
    if (partSize <= 0) {
        return "分片大小必须大于零。";
    }
    QTemporaryDir staging(QDir(directory).filePath(".zipdesk-split-XXXXXX"));
    if (!staging.isValid()) {
        return "无法在目标文件夹创建分片暂存目录。";
    }

    const QString base = QFileInfo(source).fileName();
    QStringList names;
    int part = 1;
    do {
        const QString name = QString("%1.%2").arg(base).arg(part++, 3, 10, QChar('0'));
        QSaveFile output(QDir(staging.path()).filePath(name));
        if (!output.open(QIODevice::WriteOnly)) {
            return "无法创建分片 " + name + "：" + output.errorString();
        }
        qint64 remaining = partSize;
        while (remaining > 0 && !input.atEnd()) {
            const QByteArray bytes = input.read(std::min<qint64>(remaining, 1024 * 1024));
            if (bytes.isEmpty()) {
                if (input.error() != QFileDevice::NoError) {
                    return "读取源文件失败：" + input.errorString();
                }
                break;
            }
            if (!writeFully(output, bytes)) {
                return "写入分片 " + name + " 失败：" + output.errorString();
            }
            remaining -= bytes.size();
        }
        if (input.error() != QFileDevice::NoError) {
            return "读取源文件失败：" + input.errorString();
        }
        if (!output.commit()) {
            return "保存分片 " + name + " 失败：" + output.errorString();
        }
        names.push_back(name);
    } while (!input.atEnd());

    const QDir destination(directory);
    for (const QString& name : names) {
        const QFileInfo existing(destination.filePath(name));
        if (existing.exists() || existing.isSymLink()) {
            return "目标位置已有同名分片 " + name + "。请更换目录或先移走旧分片。";
        }
    }

    QStringList installed;
    for (const QString& name : names) {
        const QString target = destination.filePath(name);
        if (!QFile::rename(QDir(staging.path()).filePath(name), target)) {
            QStringList leftovers;
            for (const QString& written : installed) {
                const QString path = destination.filePath(written);
                if (!QFile::remove(path)) {
                    leftovers.push_back(path);
                }
            }
            QString message = "无法提交分片 " + name + "；已清理本次生成的分片。";
            if (!leftovers.isEmpty()) {
                message = "无法提交分片 " + name + "；以下本次生成的文件未能清理，请手动检查：\n" + leftovers.join("\n");
            }
            return message;
        }
        installed.push_back(name);
    }
    return {};
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
    if (!m_table->selectionModel()->isRowSelected(index.row(), QModelIndex())) {
        m_table->selectRow(index.row());
    }
    m_table->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    QMenu menu(this);
    const auto label = [](const char* zh) { return zipdesk::translate(QString::fromUtf8(zh)); };
    menu.addAction(label("打开"), Qt::Key_Return, this, [this]() { openSelectedHere(); });
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
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.size() != 1 || rows.front().data(Qt::UserRole + 4).toBool()) return std::nullopt;
    const auto indices = selectedIndices();
    if (indices.size() != 1) return std::nullopt;
    const auto found = std::find_if(m_entries.begin(), m_entries.end(),
        [index = indices.front()](const auto& entry) { return entry.index == index; });
    return found == m_entries.end() ? std::nullopt : std::optional(*found);
}

std::vector<zipdesk::ArchiveEntry> MainWindow::selectedEntries() const {
    std::vector<zipdesk::ArchiveEntry> result;
    QHash<std::uint32_t, const zipdesk::ArchiveEntry*> byIndex;
    byIndex.reserve(static_cast<qsizetype>(m_entries.size()));
    for (const auto& entry : m_entries) byIndex.insert(entry.index, &entry);
    for (const std::uint32_t index : selectedIndices()) {
        if (const auto* entry = byIndex.value(index, nullptr)) result.push_back(*entry);
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
                                const std::function<void()>& after,
                                bool checkConflicts, bool allowSkip,
                                std::shared_ptr<TransferOutcome> outcome, const QString& title) {
    if (entries.empty() || m_path.empty()) {
        return;
    }
    zipdesk::ExtractRequest request;
    request.archivePath = m_path;
    request.outputDir = directory.toStdWString();
    request.password = m_password;
    std::vector<std::uint32_t> selected;
    selected.reserve(entries.size());
    for (const zipdesk::ArchiveEntry& entry : entries) {
        selected.push_back(entry.index);
    }
    request.indices = zipdesk::expandSelection(m_entries, selected);
    for (const zipdesk::ArchiveEntry& entry : m_entries) {
        if (std::find(request.indices.begin(), request.indices.end(), entry.index) != request.indices.end()) {
            request.relativePaths.push_back(entry.path);
        }
    }
    if (checkConflicts && zipdesk::hasConflicts(m_entries, request.indices, request.outputDir)) {
        const auto choice = askOverwrite(m_entries, request.indices, request.outputDir, allowSkip, &request.itemChoices);
        if (!choice) {
            return;
        }
        request.overwrite = *choice;
    }
    const bool created = !QDir(directory).exists();
    if (!QDir().mkpath(directory)) {
        QMessageBox::warning(this, "解压", "无法创建目标文件夹：\n" + directory);
        return;
    }
    request.removeOutputOnCancel = created;
    if (checkConflicts) m_extractOutputDir = directory;
    if (outcome) outcome->skippedAll = request.overwrite == zipdesk::OverwritePolicy::Skip;
    const auto interrupted = trackInterrupted();
    begin(title, [this, request, outcome, interrupted](const zipdesk::OperationHooks& hooks) {
        zipdesk::ExtractResult result = m_service.extract(request, hooks, interrupted.get());
        if (outcome) outcome->result = std::move(result);
    }, after);
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
        navigateUp();
        return;
    }
    if (current.isValid() && current.data(Qt::UserRole + 5).toBool()) {
        m_prefix = current.data(Qt::UserRole + 3).toString().toStdWString();
        m_searchEdit->clear();
        fillModel();
        statusBar()->showMessage(QString::fromStdWString(m_prefix));
        return;
    }
    const auto entry = selectedEntry();
    if (!entry) {
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
    ++m_previewGeneration;
    m_previewTimer->stop();
    if (m_previewCancel) m_previewCancel->store(true);
    m_previewHint->setText("选择文件可预览文本或图片");
    m_previewStack->setCurrentIndex(0);
    if (!m_previewButton->isChecked() || m_busy || m_job->isRunning()) return;
    const auto entry = selectedEntry();
    if (!entry || entry->isDir || entry->size > 2 * 1024 * 1024) return;
    const QString name = QString::fromStdWString(entry->name);
    if (!isTextFile(name) && !isImageFile(name)) return;
    m_previewHint->setText("正在加载预览…");
    m_previewTimer->start(160);
}

void MainWindow::startPreview() {
    if (m_previewThread != nullptr || !m_previewButton->isChecked() || m_busy || m_job->isRunning()) return;
    const auto entry = selectedEntry();
    if (!entry || entry->isDir || entry->size > 2 * 1024 * 1024) return;
    const QString name = QString::fromStdWString(entry->name);
    if (!isTextFile(name) && !isImageFile(name)) return;
    const auto generation = m_previewGeneration;
    const auto path = m_path;
    const auto password = m_password;
    const auto index = entry->index;
    const auto dll = QDir(QCoreApplication::applicationDirPath()).filePath("7z.dll").toStdWString();
    auto result = std::make_shared<QByteArray>();
    auto success = std::make_shared<bool>(false);
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_previewCancel = cancel;
    auto* thread = QThread::create([path, password, index, dll, result, success, cancel]() {
        try {
            zipdesk::ArchiveService service(dll);
            zipdesk::OperationHooks hooks;
            hooks.cancel = cancel.get();
            const auto bytes = service.readItem(path, index, password, hooks);
            *result = QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
            *success = true;
        } catch (const std::exception&) {
        }
    });
    m_previewThread = thread;
    connect(thread, &QThread::finished, this, [this, thread, result, success, generation, name]() {
        m_previewThread = nullptr;
        m_previewCancel.reset();
        if (generation != m_previewGeneration) {
            startPreview();
            return;
        }
        if (!*success) {
            m_previewHint->setText("无法预览此文件");
            return;
        }
        if (isImageFile(name)) {
            QPixmap pixmap;
            if (pixmap.loadFromData(*result)) {
                m_previewImage->setPixmap(pixmap.scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                m_previewStack->setCurrentWidget(m_previewImage);
            } else {
                m_previewHint->setText("无法预览此图片");
            }
        } else {
            m_previewText->setPlainText(QString::fromUtf8(*result));
            m_previewStack->setCurrentWidget(m_previewText);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::repairArchive() {
    if (m_path.empty()) {
        return;
    }
    auto result = std::make_shared<zipdesk::ArchiveService::RepairResult>();
    begin(
        "正在抢救可读取文件",
        [this, result](const zipdesk::OperationHooks& hooks) {
            *result = m_service.repair(m_path, m_password, hooks);
        },
        [this, result]() {
            QMessageBox::information(this, "抢救可读取文件",
                                     QString("已恢复 %1 个文件，失败 %2 个。\n%3")
                                         .arg(result->recovered)
                                         .arg(result->failed)
                                         .arg(QString::fromStdWString(result->outputDir)));
        });
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
    const std::wstring archivePath = m_path;
    const std::wstring password = m_password;
    const QString root = tempDirectory("edit-" + QUuid::createUuid().toString(QUuid::Id128));
    const QString file = entryFile(root, *entry);
    extractEntries({*entry}, root, [this, entry = *entry, file, archivePath, password]() {
        auto* process = new QProcess(this);
        connect(process, &QProcess::finished, this,
                [this, process, entry, file, archivePath, password](int, QProcess::ExitStatus status) {
            process->deleteLater();
            if (status != QProcess::NormalExit) {
                QMessageBox::warning(this, "编辑", "外部编辑器异常结束，编辑文件保留在：\n" + file);
                return;
            }
            if (m_job->isRunning()) {
                QMessageBox::warning(this, "编辑", "当前仍在执行其他操作，无法自动保存。编辑文件保留在：\n" + file);
                return;
            }
            begin(
                "正在保存修改",
                [this, entry, file, archivePath, password](const zipdesk::OperationHooks& hooks) {
                    const auto info = m_service.read(archivePath, password, hooks);
                    const auto found = std::find_if(info.entries.begin(), info.entries.end(),
                        [index = entry.index](const auto& item) { return item.index == index; });
                    if (found == info.entries.end() || found->path != entry.path) {
                        throw zipdesk::ArchiveError(zipdesk::ErrorKind::Other,
                            L"原压缩包的条目已变化，未回写。编辑文件仍保留在临时目录。");
                    }
                    m_service.replaceItem(archivePath, entry.index, file.toStdWString(), password, hooks);
                },
                [this, archivePath]() {
                    if (m_path == archivePath) {
                        reloadArchive();
                    } else {
                        statusBar()->showMessage("已保存修改到原压缩包：" + QString::fromStdWString(archivePath));
                    }
                });
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
    const QStringList selectedPaths = selectedTopLevelPaths();
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
    extractEntries(toExtract, directory, [this, selectedPaths, directory]() {
        QList<QUrl> urls;
        for (const QString& relative : selectedPaths) {
            const QString path = QDir(directory).filePath(relative);
            if (QFileInfo::exists(path)) urls.push_back(QUrl::fromLocalFile(path));
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
        QMessageBox::information(this, "青匣", "请先选择文件。");
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(this, "复制到");
    if (directory.isEmpty()) {
        return;
    }
    // Same measured counts as extraction: written / overwritten / kept / skipped / missing.
    const auto outcome = std::make_shared<TransferOutcome>();
    extractEntries(entries, directory, [this, directory, outcome]() {
        showTransferResult("复制", directory, outcome->skippedAll, outcome->result);
    }, true, true, outcome, QStringLiteral("正在复制"));
}

void MainWindow::moveSelectedTo() {
    const auto entries = selectedEntries();
    if (entries.empty()) {
        QMessageBox::information(this, "青匣", "请先选择文件。");
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(this, "移动到");
    if (directory.isEmpty()) {
        return;
    }
    if (QMessageBox::question(this, "移动所选项目",
            QString("将选中的 %1 项移动到：\n%2\n\n文件写入并核对成功后，会从压缩包中移除。")
                .arg(entries.size()).arg(QDir::toNativeSeparators(directory)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    zipdesk::ExtractRequest request;
    request.archivePath = m_path;
    request.password = m_password;
    request.outputDir = directory.toStdWString();
    for (const auto& entry : entries) {
        request.indices.push_back(entry.index);
    }
    const auto expanded = zipdesk::expandSelection(m_entries, request.indices);
    if (zipdesk::hasConflicts(m_entries, expanded, request.outputDir) && !askOverwrite(m_entries, expanded, request.outputDir, false)) return;
    m_extractOutputDir = directory;
    begin("正在移动",
        [this, request](const zipdesk::OperationHooks& hooks) { m_service.move(request, hooks); },
        [this, directory]() {
            showExtractIssue("移动完成", "所选项目已写入目标目录，并从压缩包中移除。", directory, false);
            reloadArchive();
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
        const QString error = splitFile(file, directory, static_cast<qint64>(megabytes) * 1024 * 1024);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, "分割文件", error);
            return;
        }
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
    const QString outputPath = QDir::cleanPath(QFileInfo(output).absoluteFilePath());
    for (const QString& part : parts) {
        if (QDir::cleanPath(QFileInfo(part).absoluteFilePath()).compare(outputPath, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, "合并文件", "输出文件不能是选中的分片。");
            return;
        }
    }
    QSaveFile target(output);
    if (!target.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "合并文件", "无法写入目标文件：" + target.errorString());
        return;
    }
    for (const QString& part : parts) {
        QFile input(part);
        if (!input.open(QIODevice::ReadOnly)) {
            target.cancelWriting();
            QMessageBox::warning(this, "合并文件", "无法读取分片 " + part + "：" + input.errorString());
            return;
        }
        while (!input.atEnd()) {
            const QByteArray bytes = input.read(1024 * 1024);
            if (bytes.isEmpty()) {
                if (input.error() != QFileDevice::NoError) {
                    target.cancelWriting();
                    QMessageBox::warning(this, "合并文件", "读取分片失败 " + part + "：" + input.errorString());
                    return;
                }
                break;
            }
            if (!writeFully(target, bytes)) {
                target.cancelWriting();
                QMessageBox::warning(this, "合并文件", "写入目标文件失败：" + target.errorString());
                return;
            }
        }
        if (input.error() != QFileDevice::NoError) {
            target.cancelWriting();
            QMessageBox::warning(this, "合并文件", "读取分片失败 " + part + "：" + input.errorString());
            return;
        }
    }
    if (!target.commit()) {
        QMessageBox::warning(this, "合并文件", "保存合并文件失败：" + target.errorString());
        return;
    }
    QMessageBox::information(this, "合并文件", "已合并为\n" + output);
}
