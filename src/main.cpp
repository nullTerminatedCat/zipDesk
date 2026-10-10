#include "ArchiveService.hpp"
#include "Cli.hpp"
#include "Launch.hpp"
#include "MainWindow.hpp"
#include "ShellIntegration.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setApplicationName("ZipDesk");
    QApplication::setApplicationDisplayName("日常解压");
    QApplication::setOrganizationName("ZipDesk");

    std::vector<std::wstring> launchArgs;
    launchArgs.reserve(static_cast<std::size_t>(application.arguments().size()));
    for (const QString& argument : application.arguments()) {
        launchArgs.push_back(argument.toStdWString());
    }
    zipdesk::LaunchCommand launch = zipdesk::parseLaunch(launchArgs);
    if (!zipdesk::claimShellBatch(launch)) {
        return 0;
    }
    if (launch.action == zipdesk::LaunchAction::Register) {
        zipdesk::installShellIntegration(QCoreApplication::applicationFilePath().toStdWString());
        return 0;
    }
    if (launch.action == zipdesk::LaunchAction::Unregister) {
        zipdesk::removeShellIntegration();
        return 0;
    }

    const QString dllPath = QDir(QCoreApplication::applicationDirPath()).filePath("7z.dll");
    if (!QFileInfo::exists(dllPath)) {
        QMessageBox::critical(nullptr, "日常解压", "找不到 7z.dll：\n" + dllPath);
        return 1;
    }

    std::unique_ptr<zipdesk::ArchiveService> service;
    try {
        service = std::make_unique<zipdesk::ArchiveService>(dllPath.toStdWString());
    } catch (const zipdesk::ArchiveError& error) {
        QMessageBox::critical(nullptr, "日常解压", QString::fromStdWString(error.message()));
        return 1;
    }

    if (zipdesk::isCliInvocation(launchArgs)) {
        return zipdesk::runCli(*service, launchArgs);
    }

    MainWindow window(*service, launch);
    window.resize(960, 600);
    window.show();
    return application.exec();
}
