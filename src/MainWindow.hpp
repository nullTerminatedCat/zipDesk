#pragma once

#include "ArchiveService.hpp"
#include "Launch.hpp"

#include <QList>
#include <QMainWindow>
#include <QUrl>

#include <functional>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

class ArchiveJob;
class ProgressDialog;
class QEvent;
class QAction;
class QMenu;
class QLabel;
class QListWidget;
class QLineEdit;
class QPlainTextEdit;
class QStackedWidget;
class ArchiveListModel;
class QTableView;
class QToolBar;
class QToolButton;
class QTimer;
class QThread;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(zipdesk::ArchiveService& service, zipdesk::LaunchCommand launch, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    // Filled on the worker thread by extractEntries and read in the success callback.
    struct TransferOutcome {
        zipdesk::ExtractResult result;
        bool skippedAll = false;
    };

    void buildUi();
    void runLaunch();
    void updateActions();
    void setBarItemVisible(QWidget* widget, bool visible);
    void setBusy(bool busy);
    void showArchive(zipdesk::ArchiveInfo info);
    void refreshRecentArchives();
    void fillModel();
    void updateLocationBar();
    void navigateUp();
    void openArchive(const QString& path);
    void chooseAndOpen();
    void createArchive(const QStringList& inputs);
    void finishCompress(const QString& output);
    void finishExtract(const QString& directory, bool skippedExisting = false,
                       const zipdesk::ExtractResult& result = {});
    void showTransferResult(const QString& verb, const QString& directory, bool skippedExisting,
                            const zipdesk::ExtractResult& result);
    void showExtractIssue(const QString& title, const QString& message, const QString& directory, bool failed,
                          const QString& details = {});
    // Starts tracking counts the service measures if the next extraction stops early.
    std::shared_ptr<std::optional<zipdesk::ExtractResult>> trackInterrupted();
    void returnHome();
    void showSettings();
    bool isShellAction() const;
    bool isShellExtract() const;
    void appendPaths(const QStringList& paths);
    void extractArchive(bool selectedOnly, bool namedFolder);
    void smartExtract(const QString& path);
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
    std::optional<zipdesk::OverwritePolicy> askOverwrite(const std::vector<zipdesk::ArchiveEntry>& entries,
        const std::vector<std::uint32_t>& indices, const std::wstring& outputDir, bool allowSkip = true,
        std::map<std::wstring, zipdesk::OverwritePolicy>* itemChoices = nullptr);
    std::vector<std::uint32_t> selectedIndices() const;
    QStringList selectedTopLevelPaths() const;
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
    void showDropHint(bool visible);
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
    void startPreview();
    void applyLanguage();
    std::optional<zipdesk::ArchiveEntry> selectedEntry() const;
    std::vector<zipdesk::ArchiveEntry> selectedEntries() const;
    void extractEntries(const std::vector<zipdesk::ArchiveEntry>& entries,
                        const QString& directory,
                        const std::function<void()>& after,
                        bool checkConflicts = false, bool allowSkip = true,
                        std::shared_ptr<TransferOutcome> outcome = nullptr,
                        const QString& title = QStringLiteral("正在解压"));

    zipdesk::ArchiveService& m_service;
    zipdesk::LaunchCommand m_launch;
    ArchiveJob* m_job = nullptr;
    ProgressDialog* m_progress = nullptr;
    QStackedWidget* m_stack = nullptr;
    QWidget* m_archivePage = nullptr;
    QStackedWidget* m_previewStack = nullptr;
    QPlainTextEdit* m_previewText = nullptr;
    QLabel* m_previewImage = nullptr;
    QLabel* m_previewHint = nullptr;
    QTimer* m_previewTimer = nullptr;
    QTimer* m_searchTimer = nullptr;
    QThread* m_previewThread = nullptr;
    std::shared_ptr<std::atomic_bool> m_previewCancel;
    quint64 m_previewGeneration = 0;
    QTableView* m_table = nullptr;
    QStackedWidget* m_fileStack = nullptr;
    QLabel* m_emptyResultTitle = nullptr;
    QLabel* m_emptyResultBody = nullptr;
    QWidget* m_clearSearchButton = nullptr;
    QLabel* m_archiveSummary = nullptr;
    ArchiveListModel* m_model = nullptr;
    QWidget* m_emptyPage = nullptr;
    QWidget* m_recentSection = nullptr;
    QListWidget* m_recentList = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QLabel* m_locationLabel = nullptr;
    QToolButton* m_upButton = nullptr;
    QAction* m_openAction = nullptr;
    QAction* m_newAction = nullptr;
    QToolBar* m_toolbar = nullptr;
    QToolButton* m_openButton = nullptr;
    QToolButton* m_newButton = nullptr;
    QToolButton* m_extractButton = nullptr;
    QToolButton* m_appendButton = nullptr;
    QToolButton* m_moreButton = nullptr;
    QToolButton* m_previewButton = nullptr;
    QAction* m_extractSelectedAction = nullptr;
    QAction* m_extractSelectedFolderAction = nullptr;
    QWidget* m_titleBar = nullptr;
    QLabel* m_captionTitle = nullptr;
    QLabel* m_dropHint = nullptr;
    QToolButton* m_pinButton = nullptr;
    QToolButton* m_maxButton = nullptr;
    QToolButton* m_homeButton = nullptr;
    bool m_stayOnTop = false;
    bool m_frameReady = false;
    QString m_clipboardDir;
    QMenu* m_extractMenu = nullptr;
    QMenu* m_appendMenu = nullptr;
    QMenu* m_moreMenu = nullptr;
    QAction* m_moreTestAction = nullptr;
    QAction* m_moreRepairAction = nullptr;
    QAction* m_moreCopyAction = nullptr;
    QAction* m_moreMoveAction = nullptr;
    QAction* m_moreDeleteAction = nullptr;
    QAction* m_moreInfoAction = nullptr;
    QList<QAction*> m_itemActions;
    std::wstring m_prefix;
    std::wstring m_comment;

    std::wstring m_path;
    std::wstring m_password;
    bool m_encrypted = false;
    std::vector<zipdesk::ArchiveEntry> m_entries;
    std::function<void()> m_onSuccess;
    QString m_activeOperation;
    QString m_extractOutputDir;
    std::shared_ptr<std::optional<zipdesk::ExtractResult>> m_interrupted;
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
