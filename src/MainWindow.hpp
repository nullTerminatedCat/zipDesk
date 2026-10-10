#pragma once

#include "ArchiveService.hpp"
#include "Launch.hpp"

#include <QList>
#include <QMainWindow>
#include <QUrl>

#include <functional>
#include <mutex>
#include <optional>

class ArchiveJob;
class ProgressDialog;
class QAction;
class QMenu;
class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QStandardItemModel;
class QTableView;
class QToolBar;
class QToolButton;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(zipdesk::ArchiveService& service, zipdesk::LaunchCommand launch, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void runLaunch();
    void updateActions();
    void setBarItemVisible(QWidget* widget, bool visible);
    void setBusy(bool busy);
    void showArchive(zipdesk::ArchiveInfo info);
    void fillModel();
    void openArchive(const QString& path);
    void chooseAndOpen();
    void createArchive(const QStringList& inputs);
    void finishCompress(const QString& output);
    void returnHome();
    void showSettings();
    int compressFinishMode() const;
    void appendPaths(const QStringList& paths);
    void extractArchive(bool selectedOnly, bool namedFolder);
    void shellExtract(bool namedFolder);
    void shellCompress(zipdesk::ArchiveFormat format);
    void handleDropped(const QList<QUrl>& urls);
    void dragOutSelection();
    QList<QUrl> produceDragUrls(const QString& tempDir, const std::vector<std::uint32_t>& indices);
    void begin(const QString& title,
               std::function<void(const zipdesk::OperationHooks&)> task,
               std::function<void()> onSuccess);
    void onSucceeded();
    void onFailed(const QString& message);
    void onCancelled();
    std::optional<QString> promptPassword(const QString& text);
    std::optional<zipdesk::OverwritePolicy> askOverwrite();
    std::vector<std::uint32_t> selectedIndices() const;
    void storePending(zipdesk::ArchiveInfo info);
    zipdesk::ArchiveInfo takePending();
    void toggleShellIntegration();
    void installItemShortcuts();
    void showItemMenu(const QPoint& position);
    void reloadArchive();
    void testArchive();
    void copySelectedTo();
    void copySelectedToClipboard();
    void toggleStayOnTop();
    void updateCaptionButtons();
    void moveSelectedTo();
    void deleteSelected();
    void showInfo();
    void openSelected(bool newWindow);
    void openSelectedHere();
    void viewSelected();
    void editSelected();
    void renameSelected();
    void showProperties();
    void showCrc();
    void splitSelected();
    void mergeFiles();
    void createFolder();
    void createFile();
    void showLink();
    void showStreams();
    void showComment();
    void repairArchive();
    void updatePreview();
    void applyLanguage();
    std::optional<zipdesk::ArchiveEntry> selectedEntry() const;
    std::vector<zipdesk::ArchiveEntry> selectedEntries() const;
    void extractEntries(const std::vector<zipdesk::ArchiveEntry>& entries,
                        const QString& directory,
                        const std::function<void()>& after);

    zipdesk::ArchiveService& m_service;
    zipdesk::LaunchCommand m_launch;
    ArchiveJob* m_job = nullptr;
    ProgressDialog* m_progress = nullptr;
    QStackedWidget* m_stack = nullptr;
    QWidget* m_archivePage = nullptr;
    QStackedWidget* m_previewStack = nullptr;
    QPlainTextEdit* m_previewText = nullptr;
    QLabel* m_previewImage = nullptr;
    QTableView* m_table = nullptr;
    QStandardItemModel* m_model = nullptr;
    QWidget* m_emptyPage = nullptr;
    QAction* m_openAction = nullptr;
    QAction* m_newAction = nullptr;
    QToolBar* m_toolbar = nullptr;
    QToolButton* m_openButton = nullptr;
    QToolButton* m_newButton = nullptr;
    QToolButton* m_extractButton = nullptr;
    QToolButton* m_appendButton = nullptr;
    QToolButton* m_testButton = nullptr;
    QToolButton* m_repairButton = nullptr;
    QToolButton* m_copyButton = nullptr;
    QToolButton* m_moveButton = nullptr;
    QToolButton* m_deleteButton = nullptr;
    QToolButton* m_infoButton = nullptr;
    QAction* m_extractSelectedAction = nullptr;
    QAction* m_extractSelectedFolderAction = nullptr;
    QWidget* m_titleBar = nullptr;
    QLabel* m_captionTitle = nullptr;
    QToolButton* m_pinButton = nullptr;
    QToolButton* m_maxButton = nullptr;
    QToolButton* m_homeButton = nullptr;
    bool m_stayOnTop = false;
    bool m_frameReady = false;
    QString m_clipboardDir;
    QMenu* m_extractMenu = nullptr;
    QMenu* m_appendMenu = nullptr;
    QList<QAction*> m_itemActions;
    std::wstring m_prefix;
    std::wstring m_comment;

    std::wstring m_path;
    std::wstring m_password;
    bool m_encrypted = false;
    std::vector<zipdesk::ArchiveEntry> m_entries;
    std::function<void()> m_onSuccess;
    bool m_busy = false;
    bool m_closeAfterJob = false;
    bool m_inDrag = false;
    bool m_dragDone = false;
    bool m_dragOk = false;
    QString m_dragError;
    std::mutex m_pendingMutex;
    zipdesk::ArchiveInfo m_pending;
    bool m_hasPending = false;
};
