#include "CompressDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QLabel>
#include <QPixmap>
#include <QToolButton>
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

    auto* advanced = dialog.findChild<QToolButton*>("advancedToggle");
    auto* panel = dialog.findChild<QWidget*>("advancedPanel");
    auto* password = dialog.findChild<QLineEdit*>("archivePassword");
    auto* confirm = dialog.findChild<QLineEdit*>("confirmPassword");
    if (!advanced || !panel || !password || !confirm || !panel->isHidden() || !output->isVisible()) {
        std::cerr << "FAIL: default compression settings are not compact\n";
        return 1;
    }
    advanced->click();
    if (!panel->isVisible() || !password->isVisible()) return 1;
    password->setText("sample-password");
    confirm->setText(password->text());
    advanced->click();
    if (!panel->isHidden() || dialog.request().password != L"sample-password") {
        std::cerr << "FAIL: collapsing advanced settings lost the password\n";
        return 1;
    }
    password->clear();
    confirm->clear();
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
    CompressDialog seeded;
    seed.password = L"preset";
    seed.format = zipdesk::ArchiveFormat::SevenZip;
    seed.sfx = true;
    seeded.setRequest(seed);
    auto* seededPanel = seeded.findChild<QWidget*>("advancedPanel");
    auto* seededToggle = seeded.findChild<QToolButton*>("advancedToggle");
    if (!seededPanel || !seededToggle || seededPanel->isHidden() || !seededToggle->isChecked() ||
        seeded.request().password != seed.password || !seeded.request().sfx) {
        std::cerr << "FAIL: seeded advanced options were hidden or changed\n";
        return 1;
    }
    if (qEnvironmentVariableIsSet("ZIPDESK_UI_CAPTURE")) {
        seeded.show();
        app.processEvents();
        seeded.grab().save(QDir::current().filePath("build/ui-compress-advanced.png"));
    }
    seed.password.clear();
    seed.sfx = false;
    seeded.setRequest(seed);
    if (!seededPanel->isHidden()) return 1;
    CompressDialog invalid;
    invalid.show();
    auto* error = invalid.findChild<QLabel*>("validationError");
    auto* invalidOutput = invalid.findChild<QLineEdit*>("outputPath");
    auto* invalidPassword = invalid.findChild<QLineEdit*>("archivePassword");
    auto* invalidConfirm = invalid.findChild<QLineEdit*>("confirmPassword");
    auto* invalidAdvanced = invalid.findChild<QToolButton*>("advancedToggle");
    if (!error || !invalidOutput || !invalidPassword || !invalidConfirm || !invalidAdvanced) return 1;
    static_cast<QDialog&>(invalid).accept();
    if (!error->isVisible() || invalid.result() == QDialog::Accepted) return 1;
    seed.format = zipdesk::ArchiveFormat::Zip;
    seed.password.clear();
    seed.sfx = false;
    invalid.setRequest(seed);
    if (!error->isHidden()) return 1;
    invalidOutput->clear();
    static_cast<QDialog&>(invalid).accept();
    if (!error->isVisible() || !invalidOutput->property("invalid").toBool()) return 1;
    invalidOutput->setText(customPath);
    if (!error->isHidden() || invalidOutput->property("invalid").toBool()) return 1;
    invalidPassword->setText("first");
    invalidConfirm->setText("second");
    static_cast<QDialog&>(invalid).accept();
    if (!error->isVisible() || !invalidAdvanced->isChecked() || !invalidConfirm->property("invalid").toBool()) return 1;
    if (qEnvironmentVariableIsSet("ZIPDESK_UI_CAPTURE")) {
        app.processEvents();
        invalid.grab().save(QDir::current().filePath("build/ui-compress-validation.png"));
    }
    invalidPassword->setText(QString::fromUtf8("中文"));
    invalidConfirm->setText(invalidPassword->text());
    static_cast<QDialog&>(invalid).accept();
    if (!error->isVisible() || !invalidPassword->property("invalid").toBool()) return 1;
    invalidPassword->clear();
    invalidConfirm->clear();
    invalidOutput->setText(QDir::currentPath() + "/");
    static_cast<QDialog&>(invalid).accept();
    if (!error->isVisible() || !invalidOutput->property("invalid").toBool()) return 1;
    invalidOutput->setText(customPath);
    static_cast<QDialog&>(invalid).accept();
    if (invalid.result() != QDialog::Accepted || !error->isHidden()) {
        std::cerr << "FAIL: inline validation did not recover after corrections\n";
        return 1;
    }
    std::cout << "UI SMOKE OK\n";
    return 0;
}
