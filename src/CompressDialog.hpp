#pragma once

#include "ArchiveService.hpp"

#include <QDialog>
#include <QString>
#include <QStringList>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QLabel;

QString defaultArchivePath(const QStringList& inputs, const QString& extension);

class CompressDialog : public QDialog {
    Q_OBJECT

public:
    explicit CompressDialog(QWidget* parent = nullptr);

    void setRequest(const zipdesk::CompressRequest& request);
    zipdesk::CompressRequest request() const;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void addPaths(const QStringList& paths);
    void setDragHighlight(bool active);
    void addFiles();
    void addFolder();
    void removeSelected();
    void chooseOutput();
    void updateFileCount();
    void refreshOutput();
    void updateFormatOptions();
    void accept() override;

    QListWidget* m_files = nullptr;
    QComboBox* m_format = nullptr;
    QComboBox* m_level = nullptr;
    QComboBox* m_volume = nullptr;
    QCheckBox* m_solid = nullptr;
    QCheckBox* m_sfx = nullptr;
    QLineEdit* m_password = nullptr;
    QLineEdit* m_confirm = nullptr;
    QLineEdit* m_output = nullptr;
    QLabel* m_fileCount = nullptr;
    bool m_outputCustomized = false;
};
