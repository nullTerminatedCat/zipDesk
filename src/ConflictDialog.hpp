#pragma once

#include "ArchiveService.hpp"

#include <QDialog>
#include <QString>
#include <QVector>

#include <map>
#include <string>

class QButtonGroup;
class QLabel;

// Lets the user decide overwrite / skip / keep both for each conflicting file,
// with "apply to all" shortcuts. Every row starts on "keep both", which never
// changes an existing file.
class ConflictDialog : public QDialog {
public:
    struct Item {
        std::wstring path;          // archive path, also the key in choices()
        quint64 archiveSize = 0;
        qint64 targetSize = 0;
        bool targetIsDir = false;
    };

    ConflictDialog(const QVector<Item>& items, const QString& outputDir, QWidget* parent = nullptr);

    std::map<std::wstring, zipdesk::OverwritePolicy> choices() const;
    zipdesk::OverwritePolicy choice(int row) const;
    void setChoice(int row, zipdesk::OverwritePolicy policy);
    void setAll(zipdesk::OverwritePolicy policy);
    int rowCount() const { return static_cast<int>(m_groups.size()); }

private:
    void updateSummary();

    QVector<Item> m_items;
    QVector<QButtonGroup*> m_groups;
    QLabel* m_summary = nullptr;
};
