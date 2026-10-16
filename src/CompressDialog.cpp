#include "CompressDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeData>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString extensionFor(zipdesk::ArchiveFormat format) {
    return format == zipdesk::ArchiveFormat::Zip ? ".zip" : ".7z";
}

}  // namespace

QString defaultArchivePath(const QStringList& inputs, const QString& extension) {
    if (inputs.isEmpty()) {
        return {};
    }
    const QFileInfo first(inputs.front());
    const QString directory = first.absolutePath();
    QString name;
    if (inputs.size() == 1) {
        name = first.isDir() ? first.fileName() : first.completeBaseName();
    } else {
        const bool sameDirectory = std::all_of(inputs.cbegin(), inputs.cend(), [&directory](const QString& input) {
            return QFileInfo(input).absolutePath().compare(directory, Qt::CaseInsensitive) == 0;
        });
        name = sameDirectory ? QFileInfo(directory).fileName() : QStringLiteral("新建压缩");
    }
    if (name.isEmpty()) {
        name = QStringLiteral("新建压缩");
    }
    return QDir(directory).filePath(name + extension);
}

namespace {

bool isZipPassword(const QString& password) {
    for (const QChar ch : password) {
        if (ch.unicode() < 32 || ch.unicode() >= 127) {
            return false;
        }
    }
    return true;
}

}  // namespace

CompressDialog::CompressDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("新建压缩");
    setMinimumWidth(560);
    setStyleSheet(
        "QDialog { background: #f4f8f7; color: #203b37; }"
        "QLabel#dialogTitle { color: #193b35; font-size: 19px; font-weight: 700; }"
        "QLabel#dialogHint { color: #70847d; font-size: 11px; }"
        "QLabel#sectionTitle { color: #35534d; font-weight: 600; }"
        "QListWidget { background: #ffffff; color: #294b45; border: 1px solid #d9e6e1; border-radius: 10px; padding: 5px; alternate-background-color: #f8fbfa; }"
        "QListWidget#fileList[dragOver=\"true\"] { border: 2px dashed #167468; background: #f3fbf8; }"
        "QListWidget::item { padding: 4px; }"
        "QListWidget::item:selected { background: #ddf2eb; color: #193b35; }"
        "QLineEdit, QComboBox { background: #ffffff; color: #294b45; border: 1px solid #d9e6e1; border-radius: 8px; padding: 7px 9px; min-height: 18px; }"
        "QLineEdit:focus, QComboBox:focus { border-color: #4aa18f; }"
        "QPushButton { background: #ffffff; color: #294b45; border: 1px solid #d9e6e1; border-radius: 8px; padding: 7px 13px; }"
        "QPushButton:hover { background: #f6fbf9; border-color: #8abbb0; }"
        "QPushButton#primaryButton { background: #167468; color: white; border-color: #167468; font-weight: 600; }"
        "QPushButton#primaryButton:hover { background: #0e6258; border-color: #0e6258; }"
        "QToolButton#advancedToggle { color: #167468; background: transparent; border: none; padding: 5px 0; font-weight: 600; }"
        "QCheckBox { color: #35534d; spacing: 7px; }"
        "QLabel#validationError { color: #a52a32; padding: 6px 0; }"
        "QLineEdit[invalid=\"true\"], QListWidget[invalid=\"true\"] { border: 1px solid #b64149; }");

    setAcceptDrops(true);
    m_files = new QListWidget(this);
    m_files->setObjectName("fileList");
    m_files->setAcceptDrops(false);
    m_files->setMinimumHeight(136);
    m_files->setAlternatingRowColors(true);
    m_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto* addFileButton = new QPushButton("添加文件", this);
    auto* addFolderButton = new QPushButton("添加文件夹", this);
    auto* removeButton = new QPushButton("移除", this);
    auto* fileButtons = new QHBoxLayout;
    fileButtons->addWidget(addFileButton);
    fileButtons->addWidget(addFolderButton);
    fileButtons->addWidget(removeButton);
    fileButtons->addStretch();
    m_fileCount = new QLabel("0 项", this);
    fileButtons->addWidget(m_fileCount);

    m_format = new QComboBox(this);
    m_format->setObjectName("archiveFormat");
    m_format->addItem("zip", static_cast<int>(zipdesk::ArchiveFormat::Zip));
    m_format->addItem("7z", static_cast<int>(zipdesk::ArchiveFormat::SevenZip));

    m_level = new QComboBox(this);
    m_level->addItem("存储", static_cast<int>(zipdesk::CompressionLevel::Store));
    m_level->addItem("快速", static_cast<int>(zipdesk::CompressionLevel::Fast));
    m_level->addItem("标准", static_cast<int>(zipdesk::CompressionLevel::Normal));
    m_level->addItem("最大", static_cast<int>(zipdesk::CompressionLevel::Maximum));
    m_level->setCurrentIndex(2);

    m_volume = new QComboBox(this);
    m_volume->addItem("不分卷", static_cast<qulonglong>(0));
    m_volume->addItem("10 MB", static_cast<qulonglong>(10ull * 1024 * 1024));
    m_volume->addItem("100 MB", static_cast<qulonglong>(100ull * 1024 * 1024));
    m_volume->addItem("700 MB", static_cast<qulonglong>(700ull * 1024 * 1024));
    m_solid = new QCheckBox("固实压缩（仅 7z）", this);
    m_sfx = new QCheckBox("创建自解压 exe（仅 7z）", this);
    m_sfx->setObjectName("selfExtracting");
    m_solid->setEnabled(false);
    m_sfx->setEnabled(false);

    m_password = new QLineEdit(this);
    m_password->setObjectName("archivePassword");
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText("可留空");
    m_confirm = new QLineEdit(this);
    m_confirm->setObjectName("confirmPassword");
    m_confirm->setEchoMode(QLineEdit::Password);

    m_output = new QLineEdit(this);
    m_output->setObjectName("outputPath");
    m_output->setPlaceholderText("选择保存位置和文件名");
    auto* browseButton = new QPushButton("浏览…", this);
    auto* outputRow = new QHBoxLayout;
    outputRow->addWidget(m_output, 1);
    outputRow->addWidget(browseButton);

    auto* form = new QFormLayout;
    form->setVerticalSpacing(9);
    form->setHorizontalSpacing(14);
    form->addRow("格式", m_format);
    form->addRow("将保存为", outputRow);

    auto* advancedButton = new QToolButton(this);
    advancedButton->setObjectName("advancedToggle");
    advancedButton->setText("高级选项（密码、压缩级别、分卷）");
    advancedButton->setCheckable(true);
    advancedButton->setArrowType(Qt::RightArrow);
    advancedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* advancedPanel = new QWidget(this);
    advancedPanel->setObjectName("advancedPanel");
    auto* advancedLayout = new QFormLayout(advancedPanel);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setVerticalSpacing(9);
    advancedLayout->addRow("压缩级别", m_level);
    advancedLayout->addRow("密码", m_password);
    advancedLayout->addRow("确认密码", m_confirm);
    advancedLayout->addRow("分卷", m_volume);
    advancedLayout->addRow("", m_solid);
    advancedLayout->addRow("", m_sfx);
    advancedPanel->hide();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText("压缩");
    buttons->button(QDialogButtonBox::Ok)->setObjectName("primaryButton");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(11);
    auto* title = new QLabel("创建压缩包", this);
    title->setObjectName("dialogTitle");
    auto* hint = new QLabel("将文件或文件夹拖到列表中，也可以点击「添加文件」。", this);
    hint->setObjectName("dialogHint");
    auto* section = new QLabel("文件与文件夹", this);
    section->setObjectName("sectionTitle");
    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addSpacing(6);
    layout->addWidget(section);
    layout->addWidget(m_files, 1);
    layout->addLayout(fileButtons);
    layout->addLayout(form);
    layout->addWidget(advancedButton);
    layout->addWidget(advancedPanel);
    m_validationError = new QLabel(this);
    m_validationError->setObjectName("validationError");
    m_validationError->setTextFormat(Qt::PlainText);
    m_validationError->setWordWrap(true);
    m_validationError->hide();
    layout->addWidget(m_validationError);
    layout->addWidget(buttons);
    for (QLineEdit* field : {m_output, m_password, m_confirm}) {
        connect(field, &QLineEdit::textChanged, this, &CompressDialog::clearValidationError);
    }


    connect(addFileButton, &QPushButton::clicked, this, &CompressDialog::addFiles);
    connect(addFolderButton, &QPushButton::clicked, this, &CompressDialog::addFolder);
    connect(removeButton, &QPushButton::clicked, this, &CompressDialog::removeSelected);
    connect(browseButton, &QPushButton::clicked, this, &CompressDialog::chooseOutput);
    connect(m_output, &QLineEdit::textEdited, this, [this]() { m_outputCustomized = true; });
    connect(advancedButton, &QToolButton::toggled, this, [advancedButton, advancedPanel](bool expanded) {
        advancedButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        advancedPanel->setVisible(expanded);
    });
    connect(m_format, &QComboBox::currentIndexChanged, this, &CompressDialog::updateFormatOptions);
    connect(m_sfx, &QCheckBox::toggled, this, &CompressDialog::refreshOutput);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void CompressDialog::setRequest(const zipdesk::CompressRequest& request) {
    clearValidationError();
    m_files->clear();
    m_outputCustomized = !request.outputPath.empty();
    for (const std::wstring& input : request.inputs) {
        m_files->addItem(QString::fromStdWString(input));
    }
    const int formatIndex = m_format->findData(static_cast<int>(request.format));
    if (formatIndex >= 0) {
        m_format->setCurrentIndex(formatIndex);
    }
    const int levelIndex = m_level->findData(static_cast<int>(request.level));
    if (levelIndex >= 0) {
        m_level->setCurrentIndex(levelIndex);
    }
    m_password->setText(QString::fromStdWString(request.password));
    m_confirm->setText(m_password->text());
    m_volume->setCurrentIndex(std::max(0, m_volume->findData(static_cast<qulonglong>(request.volumeSize))));
    m_solid->setChecked(request.solid);
    m_sfx->setChecked(request.sfx);
    findChild<QToolButton*>("advancedToggle")->setChecked(!request.password.empty() ||
        request.volumeSize != 0 || request.solid || request.sfx ||
        request.level != zipdesk::CompressionLevel::Normal);
    if (m_outputCustomized) {
        m_output->setText(QString::fromStdWString(request.outputPath));
    }
    refreshOutput();
    updateFileCount();
}

zipdesk::CompressRequest CompressDialog::request() const {
    zipdesk::CompressRequest result;
    for (int row = 0; row < m_files->count(); ++row) {
        result.inputs.push_back(m_files->item(row)->text().toStdWString());
    }
    result.outputPath = m_output->text().trimmed().toStdWString();
    result.format = static_cast<zipdesk::ArchiveFormat>(m_format->currentData().toInt());
    result.level = static_cast<zipdesk::CompressionLevel>(m_level->currentData().toInt());
    result.password = m_password->text().toStdWString();
    result.volumeSize = m_volume->currentData().toULongLong();
    result.solid = m_solid->isChecked();
    result.sfx = m_sfx->isChecked();
    return result;
}

void CompressDialog::setDragHighlight(bool active) {
    m_files->setProperty("dragOver", active);
    m_files->style()->unpolish(m_files);
    m_files->style()->polish(m_files);
}

void CompressDialog::addPaths(const QStringList& paths) {
    for (const QString& path : paths) {
        const QString local = QFileInfo(path).absoluteFilePath();
        if (local.isEmpty() || !QFileInfo::exists(local)) {
            continue;
        }
        if (m_files->findItems(local, Qt::MatchExactly).isEmpty()) {
            m_files->addItem(local);
        }
    }
    refreshOutput();
    updateFileCount();
}

void CompressDialog::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        setDragHighlight(true);
        return;
    }
    QDialog::dragEnterEvent(event);
}

void CompressDialog::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QDialog::dragMoveEvent(event);
}

void CompressDialog::dragLeaveEvent(QDragLeaveEvent* event) {
    setDragHighlight(false);
    QDialog::dragLeaveEvent(event);
}

void CompressDialog::dropEvent(QDropEvent* event) {
    setDragHighlight(false);
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile());
        }
    }
    if (paths.isEmpty()) {
        QDialog::dropEvent(event);
        return;
    }
    addPaths(paths);
    event->acceptProposedAction();
}

void CompressDialog::addFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, "添加文件");
    for (const QString& file : files) {
        if (m_files->findItems(file, Qt::MatchExactly).isEmpty()) {
            m_files->addItem(file);
        }
    }
    refreshOutput();
    updateFileCount();
}

void CompressDialog::addFolder() {
    const QString folder = QFileDialog::getExistingDirectory(this, "添加文件夹");
    if (!folder.isEmpty()) {
        if (m_files->findItems(folder, Qt::MatchExactly).isEmpty()) {
            m_files->addItem(folder);
        }
    }
    refreshOutput();
    updateFileCount();
}

void CompressDialog::removeSelected() {
    qDeleteAll(m_files->selectedItems());
    refreshOutput();
    updateFileCount();
}

void CompressDialog::chooseOutput() {
    const auto format = static_cast<zipdesk::ArchiveFormat>(m_format->currentData().toInt());
    const QString extension = m_sfx->isChecked() ? QStringLiteral(".exe") : extensionFor(format);
    const QString suggested = m_output->text().isEmpty()
        ? QDir(QDir::homePath()).filePath(QStringLiteral("新建压缩") + extension)
        : m_output->text();
    const QString path = QFileDialog::getSaveFileName(this, "保存压缩包", suggested,
        QString("%1 文件 (*%1)").arg(extension));
    if (!path.isEmpty()) {
        m_outputCustomized = true;
        m_output->setText(path);
        refreshOutput();
    }
}

void CompressDialog::updateFileCount() {
    clearValidationError();
    m_fileCount->setText(QString("%1 项").arg(m_files->count()));
}

void CompressDialog::refreshOutput() {
    QStringList inputs;
    for (int row = 0; row < m_files->count(); ++row) {
        inputs.push_back(m_files->item(row)->text());
    }
    const auto format = static_cast<zipdesk::ArchiveFormat>(m_format->currentData().toInt());
    const QString extension = m_sfx->isChecked() ? QStringLiteral(".exe") : extensionFor(format);
    if (!m_outputCustomized || m_output->text().isEmpty()) {
        m_output->setText(defaultArchivePath(inputs, extension));
        return;
    }
    QString path = m_output->text().trimmed();
    for (const QString& oldExtension : {QStringLiteral(".zip"), QStringLiteral(".7z"), QStringLiteral(".exe")}) {
        if (path.endsWith(oldExtension, Qt::CaseInsensitive)) {
            path.chop(oldExtension.size());
            break;
        }
    }
    m_output->setText(path + extension);
}

void CompressDialog::updateFormatOptions() {
    clearValidationError();
    const bool sevenZip = static_cast<zipdesk::ArchiveFormat>(m_format->currentData().toInt()) == zipdesk::ArchiveFormat::SevenZip;
    m_solid->setEnabled(sevenZip);
    m_sfx->setEnabled(sevenZip);
    if (!sevenZip) {
        m_solid->setChecked(false);
        m_sfx->setChecked(false);
    }
    refreshOutput();
}

void CompressDialog::clearValidationError() {
    if (m_validationError) m_validationError->hide();
    if (m_invalidField) {
        m_invalidField->setProperty("invalid", false);
        m_invalidField->style()->unpolish(m_invalidField);
        m_invalidField->style()->polish(m_invalidField);
        m_invalidField->update();
        m_invalidField = nullptr;
    }
}

void CompressDialog::showValidationError(const QString& message, QWidget* field) {
    clearValidationError();
    m_validationError->setText(message);
    m_validationError->setBuddy(field);
    m_validationError->show();
    m_invalidField = field;
    field->setProperty("invalid", true);
    field->style()->unpolish(field);
    field->style()->polish(field);
    field->update();
    field->setFocus();
}

void CompressDialog::accept() {
    clearValidationError();
    if (m_files->count() == 0) {
        showValidationError("请先添加文件或文件夹。", m_files);
        return;
    }
    if (m_output->text().trimmed().isEmpty()) {
        showValidationError("无法确定保存位置。", m_output);
        return;
    }
    if (m_password->text() != m_confirm->text()) {
        findChild<QToolButton*>("advancedToggle")->setChecked(true);
        m_confirm->setFocus();
        showValidationError("两次输入的密码不一致。", m_confirm);
        return;
    }
    const auto format = static_cast<zipdesk::ArchiveFormat>(m_format->currentData().toInt());
    if (format == zipdesk::ArchiveFormat::Zip && !m_password->text().isEmpty() && !isZipPassword(m_password->text())) {
        findChild<QToolButton*>("advancedToggle")->setChecked(true);
        m_password->setFocus();
        showValidationError("ZIP 密码只能包含英文字符、数字和符号。7z 可以包含中文密码。", m_password);
        return;
    }
    QString path = m_output->text().trimmed();
    if (QFileInfo(path).fileName().isEmpty()) {
        showValidationError("请填写压缩包文件名。", m_output);
        return;
    }
    const QString extension = m_sfx->isChecked() ? QStringLiteral(".exe") : extensionFor(format);
    if (!path.endsWith(extension, Qt::CaseInsensitive)) {
        for (const QString& oldExtension : {QStringLiteral(".zip"), QStringLiteral(".7z"), QStringLiteral(".exe")}) {
            if (path.endsWith(oldExtension, Qt::CaseInsensitive)) {
                path.chop(oldExtension.size());
                break;
            }
        }
        path += extension;
        m_output->setText(path);
    }
    QDialog::accept();
}
