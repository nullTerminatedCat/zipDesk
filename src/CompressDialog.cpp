#include "CompressDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString extensionFor(zipdesk::ArchiveFormat format) {
    return format == zipdesk::ArchiveFormat::Zip ? ".zip" : ".7z";
}

QString parentDirectoryName(const QString& input) {
    const QFileInfo info(input);
    if (info.isDir()) {
        const QString parentPath = info.absolutePath();
        const QString parentName = QFileInfo(parentPath).fileName();
        if (!parentName.isEmpty()) {
            return parentName;
        }
        return info.fileName();
    }
    const QString parentName = QFileInfo(info.absolutePath()).fileName();
    if (!parentName.isEmpty()) {
        return parentName;
    }
    return info.completeBaseName();
}

QString parentDirectoryPath(const QString& input) {
    const QFileInfo info(input);
    if (info.isDir()) {
        const QString parentPath = info.absolutePath();
        if (QFileInfo(parentPath).fileName().isEmpty()) {
            return parentPath;
        }
        return parentPath;
    }
    return info.absolutePath();
}

}  // namespace

QString defaultArchivePath(const QStringList& inputs, const QString& extension) {
    if (inputs.isEmpty()) {
        return {};
    }
    const QString input = inputs.front();
    const QFileInfo info(input);
    QString directory = parentDirectoryPath(input);
    QString name = parentDirectoryName(input);
    if (info.isDir() && QFileInfo(info.absolutePath()).fileName().isEmpty()) {
        directory = info.absolutePath();
        name = info.fileName();
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
    setMinimumWidth(520);

    m_files = new QListWidget(this);
    auto* addFileButton = new QPushButton("添加文件", this);
    auto* addFolderButton = new QPushButton("添加文件夹", this);
    auto* removeButton = new QPushButton("移除", this);
    auto* fileButtons = new QHBoxLayout;
    fileButtons->addWidget(addFileButton);
    fileButtons->addWidget(addFolderButton);
    fileButtons->addWidget(removeButton);
    fileButtons->addStretch();

    m_format = new QComboBox(this);
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
    m_solid->setEnabled(false);
    m_sfx->setEnabled(false);

    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText("可留空");
    m_confirm = new QLineEdit(this);
    m_confirm->setEchoMode(QLineEdit::Password);

    m_output = new QLineEdit(this);
    m_output->setReadOnly(true);
    auto* outputRow = new QHBoxLayout;
    outputRow->addWidget(m_output, 1);

    auto* form = new QFormLayout;
    form->addRow("格式", m_format);
    form->addRow("压缩级别", m_level);
    form->addRow("分卷", m_volume);
    form->addRow("", m_solid);
    form->addRow("", m_sfx);
    form->addRow("密码", m_password);
    form->addRow("确认密码", m_confirm);
    form->addRow("将保存为", outputRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText("压缩");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("要压缩的文件和文件夹", this));
    layout->addWidget(m_files, 1);
    layout->addLayout(fileButtons);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(addFileButton, &QPushButton::clicked, this, &CompressDialog::addFiles);
    connect(addFolderButton, &QPushButton::clicked, this, &CompressDialog::addFolder);
    connect(removeButton, &QPushButton::clicked, this, &CompressDialog::removeSelected);
    connect(m_format, &QComboBox::currentIndexChanged, this, &CompressDialog::updateFormatOptions);
    connect(m_sfx, &QCheckBox::toggled, this, &CompressDialog::refreshOutput);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void CompressDialog::setRequest(const zipdesk::CompressRequest& request) {
    m_files->clear();
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
    refreshOutput();
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

void CompressDialog::addFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, "添加文件");
    for (const QString& file : files) {
        m_files->addItem(file);
    }
    refreshOutput();
}

void CompressDialog::addFolder() {
    const QString folder = QFileDialog::getExistingDirectory(this, "添加文件夹");
    if (!folder.isEmpty()) {
        m_files->addItem(folder);
    }
    refreshOutput();
}

void CompressDialog::removeSelected() {
    qDeleteAll(m_files->selectedItems());
    refreshOutput();
}

void CompressDialog::refreshOutput() {
    QStringList inputs;
    for (int row = 0; row < m_files->count(); ++row) {
        inputs.push_back(m_files->item(row)->text());
    }
    const auto format = static_cast<zipdesk::ArchiveFormat>(m_format->currentData().toInt());
    const QString extension = m_sfx->isChecked() ? QStringLiteral(".exe") : extensionFor(format);
    m_output->setText(defaultArchivePath(inputs, extension));
}

void CompressDialog::updateFormatOptions() {
    const bool sevenZip = static_cast<zipdesk::ArchiveFormat>(m_format->currentData().toInt()) == zipdesk::ArchiveFormat::SevenZip;
    m_solid->setEnabled(sevenZip);
    m_sfx->setEnabled(sevenZip);
    if (!sevenZip) {
        m_solid->setChecked(false);
        m_sfx->setChecked(false);
    }
    refreshOutput();
}

void CompressDialog::accept() {
    if (m_files->count() == 0) {
        QMessageBox::warning(this, "新建压缩", "请先添加文件或文件夹。");
        return;
    }
    refreshOutput();
    if (m_output->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "新建压缩", "无法确定保存位置。");
        return;
    }
    if (m_password->text() != m_confirm->text()) {
        QMessageBox::warning(this, "新建压缩", "两次输入的密码不一致。");
        return;
    }
    const auto format = static_cast<zipdesk::ArchiveFormat>(m_format->currentData().toInt());
    if (format == zipdesk::ArchiveFormat::Zip && !m_password->text().isEmpty() && !isZipPassword(m_password->text())) {
        QMessageBox::warning(this, "新建压缩", "ZIP 密码只能包含英文字符、数字和符号。7z 可以包含中文密码。");
        return;
    }
    QString path = m_output->text().trimmed();
    const QString extension = extensionFor(format);
    if (!path.endsWith(extension, Qt::CaseInsensitive)) {
        path += extension;
        m_output->setText(path);
    }
    QDialog::accept();
}
