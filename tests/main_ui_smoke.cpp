#include "ArchiveService.hpp"
#include "Launch.hpp"
#include "MainWindow.hpp"

#include <bit7z/bit7zlibrary.hpp>
#include <bit7z/bitfilecompressor.hpp>
#include <bit7z/bitformat.hpp>

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QItemSelectionModel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QTest>

#include <iostream>
#include <map>
#include <vector>

namespace {

bool writeLargeZip(const QString& path, int count) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    struct CentralRecord { QByteArray name; quint32 offset; };
    std::vector<CentralRecord> records;
    records.reserve(count);
    for (int i = 0; i < count; ++i) {
        const QByteArray name = QString("item_%1.txt").arg(i, 5, 10, QChar('0')).toUtf8();
        records.push_back({name, static_cast<quint32>(file.pos())});
        stream << quint32(0x04034b50) << quint16(20) << quint16(0) << quint16(0)
               << quint16(0) << quint16(0) << quint32(0) << quint32(0) << quint32(0)
               << quint16(name.size()) << quint16(0);
        stream.writeRawData(name.constData(), name.size());
    }
    const quint32 centralOffset = static_cast<quint32>(file.pos());
    for (const auto& record : records) {
        stream << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0)
               << quint16(0) << quint16(0) << quint16(0) << quint32(0) << quint32(0)
               << quint32(0) << quint16(record.name.size()) << quint16(0) << quint16(0)
               << quint16(0) << quint16(0) << quint32(0) << record.offset;
        stream.writeRawData(record.name.constData(), record.name.size());
    }
    const quint32 centralSize = static_cast<quint32>(file.pos()) - centralOffset;
    stream << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(count)
           << quint16(count) << centralSize << centralOffset << quint16(0);
    return stream.status() == QDataStream::Ok;
}

bool testExtractResult(QApplication& app, const QString& root, const QString& scenario) {
    app.setQuitOnLastWindowClosed(false);
    const bool smart = scenario.startsWith("smart-");
    QString mode = smart ? scenario.mid(6) : scenario;
    const bool windowEntry = mode == "window";
    const bool directoryLayout = mode == "directory" || mode == "directory-skip";
    if (mode == "directory-skip") mode = "skip";
    if (windowEntry) mode = "success";
    zipdesk::ArchiveService service(
        QDir(QCoreApplication::applicationDirPath()).filePath("7z.dll").toStdWString());
    const auto write = [](const QString& path, const QByteArray& contents) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    };
    const QString input = QDir(root).filePath("sample.txt");
    const QString fresh = QDir(root).filePath("fresh.txt");
    const QString output = mode == "empty" ? root : QDir(root).filePath(directoryLayout ? "payload" : "sample");
    const QString target = QDir(output).filePath("sample.txt");
    if (!write(input, "sample") || !write(fresh, "fresh")) return false;
    zipdesk::CompressRequest request;
    request.inputs = {input.toStdWString(), fresh.toStdWString()};
    request.outputPath = QDir(root).filePath("sample.zip").toStdWString();
    if (directoryLayout) {
        const QString sourceFolder = QDir(root).filePath("input/payload");
        if (!QDir().mkpath(sourceFolder) ||
            !QFile::copy(input, QDir(sourceFolder).filePath("sample.txt")) ||
            !QFile::copy(fresh, QDir(sourceFolder).filePath("fresh.txt"))) return false;
        request.inputs = {sourceFolder.toStdWString()};
    }
    if (mode == "empty") {
        if (!writeLargeZip(QString::fromStdWString(request.outputPath), 0)) return false;
    } else {
        service.compress(request, {});
    }
    if (mode == "skip" || mode == "cancel") {
        if (!QDir().mkpath(output) || !write(target, "existing")) return false;
    } else if (mode == "failure") {
        if (!write(output, "keep")) return false;
    } else if (mode != "success" && mode != "directory" && mode != "empty") return false;

    bool conflictSeen = false;
    bool resultSeen = false;
    bool valid = true;
    const QString expectedTitle = mode == "cancel" ? "解压已取消"
        : (mode == "failure" || mode == "empty") ? "解压未完成" : "解压完成";
    zipdesk::LaunchCommand launch;
    launch.action = windowEntry ? zipdesk::LaunchAction::Open : smart ? zipdesk::LaunchAction::SmartExtract : zipdesk::LaunchAction::ExtractToFolder;
    launch.paths = {request.outputPath};
    MainWindow window(service, launch);
    bool windowActionStarted = false;
    QTimer dialogs;
    dialogs.setInterval(20);
    QObject::connect(&dialogs, &QTimer::timeout, &window, [&]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) {
            auto* table = window.findChild<QTableView*>();
            if (windowEntry && !windowActionStarted && table && table->model()->rowCount() > 0) {
                for (auto* action : window.findChildren<QAction*>()) {
                    if (action->property("i18n").toString() == "智能解压") {
                        windowActionStarted = true;
                        action->trigger();
                        break;
                    }
                }
            }
            return;
        }
        if (box->windowTitle() == "同名文件") {
            conflictSeen = true;
            const QString choice = mode == "skip" ? "全部跳过" : "取消";
            if (mode == "cancel")
                valid = write(QDir(output).filePath("user.txt"), "user") && valid;
            for (auto* button : box->buttons()) {
                if (button->text() == choice) {
                    button->click();
                    return;
                }
            }
            valid = false;
            box->reject();
            return;
        }
        resultSeen = true;
        valid = valid && box->windowTitle() == expectedTitle &&
            box->text().contains(QDir::toNativeSeparators(output));
        if (mode == "skip") valid = valid && box->text().contains("全部跳过");
        bool hasOpen = false;
        for (auto* button : box->buttons())
            if (button->text() == "打开目标文件夹") hasOpen = true;
        valid = valid && hasOpen == (mode != "failure");
        box->accept();
        if (windowEntry) QTimer::singleShot(0, &app, &QCoreApplication::quit);
    });
    dialogs.start();
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &app, [&]() {
        valid = false;
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->reject();
        app.exit(1);
    });
    watchdog.start(15000);
    const int code = app.exec();
    dialogs.stop();
    watchdog.stop();
    const auto read = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    valid = valid && code == 0 && resultSeen &&
        conflictSeen == (mode == "skip" || mode == "cancel");
    if (mode == "failure") {
        valid = valid && read(output) == "keep";
    } else if (mode == "empty") {
        valid = valid && !QFile::exists(QDir(root).filePath("sample"));
    } else {
        valid = valid && read(target) == ((mode == "success" || mode == "directory") ? "sample" : "existing");
        if (mode == "cancel") {
            valid = valid && read(QDir(output).filePath("user.txt")) == "user" &&
                !QFile::exists(QDir(output).filePath("fresh.txt"));
        } else {
            valid = valid && read(QDir(output).filePath("fresh.txt")) == "fresh";
        }
    }
    std::cout << (valid ? "EXTRACT UI OK: " : "FAIL: extract UI ") << scenario.toStdString() << "\n";
    return valid;
}
}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("ZipDeskUiSmoke");
    app.setOrganizationName("ZipDeskUiSmoke");
    QTemporaryDir temporary(QDir::current().filePath("build/main-ui-smoke-XXXXXX"));
    if (!temporary.isValid()) {
        std::cerr << "FAIL: cannot create UI test directory\n";
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings().setValue("ui/previewVisible", false);
    if (app.arguments().size() == 3 && app.arguments().at(1) == "--extract")
        return testExtractResult(app, temporary.path(), app.arguments().at(2)) ? 0 : 1;

    QFile source(QDir(temporary.path()).filePath("sample.txt"));
    if (!source.open(QIODevice::WriteOnly) || source.write("sample") != 6) {
        std::cerr << "FAIL: cannot write UI test input\n";
        return 1;
    }
    source.close();
    QFile second(QDir(temporary.path()).filePath("second.txt"));
    if (!second.open(QIODevice::WriteOnly) || second.write("second") != 6) {
        std::cerr << "FAIL: cannot write second UI test input\n";
        return 1;
    }
    second.close();
    const QString nestedPath = QDir(temporary.path()).filePath("nested");
    QDir().mkpath(nestedPath);
    QFile nested(QDir(nestedPath).filePath("inside.txt"));
    if (!nested.open(QIODevice::WriteOnly) || nested.write("inside") != 6) {
        std::cerr << "FAIL: cannot write nested UI test input\n";
        return 1;
    }
    nested.close();
    zipdesk::ArchiveService service(
        QDir(QCoreApplication::applicationDirPath()).filePath("7z.dll").toStdWString());
    zipdesk::CompressRequest request;
    request.inputs = {source.fileName().toStdWString(), second.fileName().toStdWString(), nestedPath.toStdWString()};
    request.outputPath = QDir(temporary.path()).filePath("sample.zip").toStdWString();
    service.compress(request, {});

    zipdesk::LaunchCommand launch;
    launch.action = zipdesk::LaunchAction::Open;
    launch.paths = {request.outputPath};
    MainWindow window(service, launch);
    window.resize(960, 600);
    window.show();
    auto* table = window.findChild<QTableView*>();
    QElapsedTimer timer;
    timer.start();
    while (table != nullptr && table->model()->rowCount() == 0 && timer.elapsed() < 5000) {
        QTest::qWait(20);
    }
    if (table == nullptr || table->model()->rowCount() != 3) {
        std::cerr << "FAIL: sample archive did not open\n";
        return 1;
    }

    const auto button = [&window](const QString& key) -> QToolButton* {
        for (QToolButton* item : window.findChildren<QToolButton*>()) {
            if (item->property("i18n").toString() == key) {
                return item;
            }
        }
        return nullptr;
    };
    auto* extract = button("解压");
    auto* more = button("工具");
    auto* preview = button("预览");
    auto* back = button("首页");
    if (!extract || !extract->isVisible() || !more || !more->isVisible() ||
        !preview || !preview->isVisible() || preview->isChecked() ||
        !back || !back->isVisible()) {
        std::cerr << "FAIL: archive toolbar state is incorrect\n";
        return 1;
    }
    if (more->x() == 0 || more->width() < 20) {
        std::cerr << "FAIL: more menu has no toolbar geometry\n";
        return 1;
    }
    if (button("打开")->icon().isNull() || more->icon().isNull() ||
        button("打开")->toolButtonStyle() != Qt::ToolButtonTextBesideIcon) {
        std::cerr << "FAIL: toolbar icons are missing\n";
        return 1;
    }
    const QImage openGlyph = button("打开")->icon().pixmap(20, 20).toImage();
    int glyphPixels = 0;
    for (int y = 0; y < openGlyph.height(); ++y)
        for (int x = 0; x < openGlyph.width(); ++x)
            if (qAlpha(openGlyph.pixel(x, y)) > 0) ++glyphPixels;
    if (glyphPixels < 12) {
        std::cerr << "FAIL: toolbar icon has no painted pixels\n";
        return 1;
    }
    if (preview->width() < 20 || preview->text().isEmpty() || preview->x() == 0) {
        std::cerr << "FAIL: preview control has no visible size or label\n";
        return 1;
    }
    if (qEnvironmentVariableIsSet("ZIPDESK_UI_CAPTURE")) {
        window.grab().save(QDir::current().filePath("build/ui-archive.png"));
    }

    auto* search = window.findChild<QLineEdit*>("archiveSearch");
    auto* up = window.findChild<QToolButton*>("locationUp");
    if (!search || !up || up->isEnabled()) {
        std::cerr << "FAIL: archive location controls are incorrect\n";
        return 1;
    }
    int folderRow = -1;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, 0).data(Qt::UserRole + 5).toBool()) folderRow = row;
    }
    if (folderRow < 0) {
        std::cerr << "FAIL: nested directory is not visible\n";
        return 1;
    }
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
        table->visualRect(table->model()->index(folderRow, 0)).center());
    QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
        table->visualRect(table->model()->index(folderRow, 0)).center());
    bool insideVisible = false;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, 0).data().toString() == "inside.txt") insideVisible = true;
    }
    if (!up->isEnabled() || table->model()->rowCount() != 2 || !insideVisible) {
        std::cerr << "FAIL: directory navigation did not show direct children: up=" << up->isEnabled()
                  << " rows=" << table->model()->rowCount()
                  << " child=" << table->model()->index(1, 0).data().toString().toStdString() << "\n";
        return 1;
    }
    search->setText("second");
    if (table->model()->rowCount() != 1 ||
        !table->model()->index(0, 0).data().toString().contains("second.txt")) {
        std::cerr << "FAIL: archive-wide search did not find root file\n";
        return 1;
    }
    search->clear();
    QTest::mouseClick(up, Qt::LeftButton);
    if (table->model()->rowCount() != 3 || up->isEnabled()) {
        std::cerr << "FAIL: navigating up did not restore root\n";
        return 1;
    }
    search->setText("inside");
    if (table->model()->rowCount() != 1 ||
        !table->model()->index(0, 0).data().toString().contains("inside.txt")) {
        std::cerr << "FAIL: archive-wide search did not find nested file\n";
        return 1;
    }
    search->clear();

    preview->setChecked(true);
    auto* previewText = window.findChild<QPlainTextEdit*>();
    int sampleRow = -1;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, 0).data().toString() == "sample.txt") sampleRow = row;
    }
    if (!previewText || sampleRow < 0) {
        std::cerr << "FAIL: preview controls are missing\n";
        return 1;
    }
    timer.restart();
    table->selectRow(sampleRow);
    if (timer.elapsed() > 250) {
        std::cerr << "FAIL: preview selection blocked the UI\n";
        return 1;
    }
    timer.restart();
    while (previewText->toPlainText() != "sample" && timer.elapsed() < 5000) QTest::qWait(20);
    if (previewText->toPlainText() != "sample") {
        std::cerr << "FAIL: background text preview did not load\n";
        return 1;
    }
    int secondRow = -1;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, 0).data().toString() == "second.txt") secondRow = row;
    }
    table->selectRow(secondRow);
    timer.restart();
    while (previewText->toPlainText() != "second" && timer.elapsed() < 5000) QTest::qWait(20);
    if (previewText->toPlainText() != "second") {
        std::cerr << "FAIL: preview did not follow changed selection\n";
        return 1;
    }
    previewText->clear();
    table->selectRow(sampleRow);
    QTest::qWait(180);
    table->selectRow(secondRow);
    timer.restart();
    while (previewText->toPlainText() != "second" && timer.elapsed() < 5000) QTest::qWait(20);
    QTest::qWait(100);
    if (previewText->toPlainText() != "second") {
        std::cerr << "FAIL: stale preview replaced the current selection\n";
        return 1;
    }
    preview->setChecked(false);

    table->selectRow(1);
    table->selectionModel()->select(table->model()->index(2, 0),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    if (table->selectionModel()->selectedRows().size() != 2) {
        std::cerr << "FAIL: cannot select two archive rows\n";
        return 1;
    }
    QTimer::singleShot(50, &window, []() {
        if (auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget())) popup->close();
    });
    QTest::mouseClick(table->viewport(), Qt::RightButton, Qt::NoModifier,
        table->visualRect(table->model()->index(1, 0)).center());
    if (table->selectionModel()->selectedRows().size() != 2) {
        std::cerr << "FAIL: right-click cleared the archive selection\n";
        return 1;
    }

    bool settingsLanguageExclusive = false;
    QToolButton* settings = nullptr;
    for (QToolButton* item : window.findChildren<QToolButton*>()) {
        if (item->property("tip").toString() == "设置") {
            settings = item;
            break;
        }
    }
    if (!settings) {
        std::cerr << "FAIL: settings button is missing\n";
        return 1;
    }
    QTimer::singleShot(50, &window, [&settingsLanguageExclusive]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        QRadioButton* chinese = nullptr;
        QRadioButton* english = nullptr;
        for (QRadioButton* radio : dialog->findChildren<QRadioButton*>()) {
            if (radio->text() == "简体中文") chinese = radio;
            if (radio->text() == "English") english = radio;
        }
        if (chinese && english) {
            english->setChecked(true);
            settingsLanguageExclusive = english->isChecked() && !chinese->isChecked();
        }
        dialog->reject();
    });
    QTest::mouseClick(settings, Qt::LeftButton);
    if (!settingsLanguageExclusive) {
        std::cerr << "FAIL: settings language choice is not exclusive\n";
        return 1;
    }

    QTest::mouseClick(back, Qt::LeftButton);
    auto* recent = window.findChild<QListWidget*>("recentArchives");
    if (!button("打开")->isVisible() || !button("新建压缩")->isVisible() ||
        !button("解压")->isVisible() || more->isVisible() ||
        !recent || recent->count() != 1 || !recent->isVisible()) {
        std::cerr << "FAIL: home toolbar state is incorrect\n";
        return 1;
    }
    if (qEnvironmentVariableIsSet("ZIPDESK_UI_CAPTURE")) {
        window.grab().save(QDir::current().filePath("build/ui-home.png"));
    }
    auto* hero = window.findChild<QFrame*>("heroCard");
    window.resize(720, 520);
    QTest::qWait(40);
    if (!hero || hero->width() > hero->parentWidget()->width() ||
        recent->parentWidget()->width() > recent->parentWidget()->parentWidget()->width()) {
        std::cerr << "FAIL: home layout overflows at minimum window size\n";
        return 1;
    }
    if (qEnvironmentVariableIsSet("ZIPDESK_UI_CAPTURE")) {
        window.grab().save(QDir::current().filePath("build/ui-home-small.png"));
    }
    window.resize(960, 600);
    QTest::qWait(40);
    const QPoint recentPoint = recent->visualItemRect(recent->item(0)).center();
    QTest::mouseClick(recent->viewport(), Qt::LeftButton, Qt::NoModifier, recentPoint);
    QTest::mouseDClick(recent->viewport(), Qt::LeftButton, Qt::NoModifier, recentPoint);
    timer.restart();
    while (!back->isVisible() && timer.elapsed() < 5000) {
        QTest::qWait(20);
    }
    if (!back->isVisible() || table->model()->rowCount() != 3) {
        std::cerr << "FAIL: recent archive did not reopen\n";
        return 1;
    }
    QTest::mouseClick(back, Qt::LeftButton);
    QPushButton* clearRecent = nullptr;
    for (QPushButton* item : window.findChildren<QPushButton*>()) {
        if (item->property("i18n").toString() == "清空记录") {
            clearRecent = item;
            break;
        }
    }
    if (!clearRecent) {
        std::cerr << "FAIL: clear history control is missing\n";
        return 1;
    }
    QTest::mouseClick(clearRecent, Qt::LeftButton);
    if (recent->count() != 0 || recent->isVisible()) {
        std::cerr << "FAIL: recent archive history was not cleared\n";
        return 1;
    }
    const QString implicitZip = QDir(temporary.path()).filePath("implicit.zip");
    bit7z::Bit7zLibrary library(QDir(QCoreApplication::applicationDirPath()).filePath("7z.dll").toStdWString());
    bit7z::BitFileCompressor compressor(library, bit7z::BitFormat::Zip);
    compressor.compress(std::map<bit7z::tstring, bit7z::tstring>{
        {source.fileName().toStdWString(), L"virtual/child.txt"}
    }, implicitZip.toStdWString());
    const auto implicitInfo = service.read(implicitZip.toStdWString(), L"", {});
    if (implicitInfo.entries.size() != 1 || implicitInfo.entries.front().isDir) {
        std::cerr << "FAIL: implicit-directory test archive was not created correctly\n";
        return 1;
    }
    zipdesk::LaunchCommand implicitLaunch;
    implicitLaunch.action = zipdesk::LaunchAction::Open;
    implicitLaunch.paths = {implicitZip.toStdWString()};
    MainWindow implicitWindow(service, implicitLaunch);
    implicitWindow.show();
    auto* implicitTable = implicitWindow.findChild<QTableView*>();
    timer.restart();
    while (implicitTable && implicitTable->model()->rowCount() == 0 && timer.elapsed() < 5000) QTest::qWait(20);
    if (!implicitTable || implicitTable->model()->rowCount() != 1 ||
        !implicitTable->model()->index(0, 0).data(Qt::UserRole + 4).toBool()) {
        std::cerr << "FAIL: implicit directory was not shown as a folder\n";
        return 1;
    }
    if (!(implicitTable->model()->index(0, 0).flags() & Qt::ItemIsDragEnabled)) {
        std::cerr << "FAIL: virtual folder cannot be dragged\n";
        return 1;
    }
    QTest::mouseClick(implicitTable->viewport(), Qt::LeftButton, Qt::NoModifier,
        implicitTable->visualRect(implicitTable->model()->index(0, 0)).center());
    QTest::mouseDClick(implicitTable->viewport(), Qt::LeftButton, Qt::NoModifier,
        implicitTable->visualRect(implicitTable->model()->index(0, 0)).center());
    bool childVisible = false;
    for (int row = 0; row < implicitTable->model()->rowCount(); ++row) {
        if (implicitTable->model()->index(row, 0).data().toString() == "child.txt") childVisible = true;
    }
    if (!childVisible) {
        std::cerr << "FAIL: implicit directory did not open\n";
        return 1;
    }
    constexpr int largeCount = 4000;
    const QString largeZip = QDir(temporary.path()).filePath("large.zip");
    if (!writeLargeZip(largeZip, largeCount)) {
        std::cerr << "FAIL: cannot create large archive fixture\n";
        return 1;
    }
    zipdesk::LaunchCommand largeLaunch;
    largeLaunch.action = zipdesk::LaunchAction::Open;
    largeLaunch.paths = {largeZip.toStdWString()};
    MainWindow largeWindow(service, largeLaunch);
    largeWindow.show();
    auto* largeTable = largeWindow.findChild<QTableView*>();
    timer.restart();
    while (largeTable && largeTable->model()->rowCount() != largeCount && timer.elapsed() < 10000) QTest::qWait(20);
    const auto loadMs = timer.elapsed();
    if (!largeTable || largeTable->model()->rowCount() != largeCount) {
        std::cerr << "FAIL: large archive did not load\n";
        return 1;
    }
    auto* largeSearch = largeWindow.findChild<QLineEdit*>("archiveSearch");
    timer.restart();
    largeSearch->setText("item_03999");
    while (largeTable->model()->rowCount() != 1 && timer.elapsed() < 3000) QTest::qWait(20);
    const auto searchMs = timer.elapsed();
    if (largeTable->model()->rowCount() != 1 ||
        largeTable->model()->index(0, 0).data().toString() != "item_03999.txt") {
        std::cerr << "FAIL: large archive search did not update the model\n";
        return 1;
    }
    largeSearch->clear();
    timer.restart();
    while (largeTable->model()->rowCount() != largeCount && timer.elapsed() < 3000) QTest::qWait(20);
    if (largeTable->model()->rowCount() != largeCount) {
        std::cerr << "FAIL: large archive search did not restore rows\n";
        return 1;
    }
    largeTable->sortByColumn(0, Qt::DescendingOrder);
    if (largeTable->model()->index(0, 0).data().toString() != "item_03999.txt") {
        std::cerr << "FAIL: large archive sorting is incorrect\n";
        return 1;
    }
    std::cout << "LARGE UI " << largeCount << " rows: load " << loadMs
              << " ms, search " << searchMs << " ms\n";
    std::cout << "MAIN UI SMOKE OK\n";
    return 0;
}
