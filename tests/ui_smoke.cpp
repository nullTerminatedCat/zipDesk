#include "CompressDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QPixmap>
#include <QtTest/QTest>

#include <iostream>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const QString iconPath = QDir::current().filePath("resources/icon.png");
    if (defaultArchivePath({iconPath}, ".zip") != QDir::current().filePath("resources/icon.zip") ||
        defaultArchivePath({QDir::current().filePath("resources")}, ".7z") !=
            QDir::current().filePath("resources.7z") ||
        defaultArchivePath({iconPath, QDir::current().filePath("resources/empty.png")}, ".zip") !=
            QDir::current().filePath("resources/resources.zip")) {
        std::cerr << "FAIL: suggested archive name is incorrect\n";
        return 1;
    }
    CompressDialog dialog;
    zipdesk::CompressRequest seed;
    seed.inputs.push_back(iconPath.toStdWString());
    dialog.setRequest(seed);
    dialog.show();
    app.processEvents();
    if (qEnvironmentVariableIsSet("ZIPDESK_UI_CAPTURE")) {
        dialog.grab().save(QDir::current().filePath("build/ui-compress-dialog.png"));
    }

    auto* output = dialog.findChild<QLineEdit*>("outputPath");
    auto* format = dialog.findChild<QComboBox*>("archiveFormat");
    auto* sfx = dialog.findChild<QCheckBox*>("selfExtracting");
    if (!output || !format || !sfx || !output->text().endsWith("icon.zip")) {
        std::cerr << "FAIL: compression dialog did not initialize\n";
        return 1;
    }

    output->setFocus();
    QTest::keyClick(output, Qt::Key_A, Qt::ControlModifier);
    const QString customPath = QDir::current().filePath("custom-name.zip");
    QTest::keyClicks(output, customPath);
    if (!output->text().endsWith("custom-name.zip")) {
        std::cerr << "FAIL: custom output path is not editable\n";
        return 1;
    }

    format->setCurrentIndex(format->findData(static_cast<int>(zipdesk::ArchiveFormat::SevenZip)));
    if (!output->text().endsWith("custom-name.7z")) {
        std::cerr << "FAIL: format change lost the custom output path\n";
        return 1;
    }
    sfx->setChecked(true);
    if (!output->text().endsWith("custom-name.exe")) {
        std::cerr << "FAIL: self-extracting output did not use .exe\n";
        return 1;
    }
    format->setCurrentIndex(format->findData(static_cast<int>(zipdesk::ArchiveFormat::Zip)));
    if (sfx->isChecked() || !output->text().endsWith("custom-name.zip")) {
        std::cerr << "FAIL: switching back to ZIP kept the self-extracting suffix\n";
        return 1;
    }
    format->setCurrentIndex(format->findData(static_cast<int>(zipdesk::ArchiveFormat::SevenZip)));
    sfx->setChecked(true);

    static_cast<QDialog&>(dialog).accept();
    if (dialog.result() != QDialog::Accepted ||
        dialog.request().outputPath != QDir::current().filePath("custom-name.exe").toStdWString() ||
        !dialog.request().sfx) {
        std::cerr << "FAIL: accepted request has the wrong output path\n";
        return 1;
    }
    std::cout << "UI SMOKE OK\n";
    return 0;
}
