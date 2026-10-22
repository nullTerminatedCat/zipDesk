#pragma once

#include <QDialog>
#include <QString>
#include <vector>
#include <string>

// Asks what to do when the user drops multiple archives, or mixes archives with
// ordinary files. Cancel leaves the choice unset.
class MultiDropDialog : public QDialog {
public:
    enum class Choice {
        Cancel = 0,
        SmartExtractAll,
        ExtractToFolders,
        CreateArchive,
        OpenFirst,
    };

    MultiDropDialog(int archiveCount, int otherCount, QWidget* parent = nullptr);

    Choice choice() const { return m_choice; }

private:
    void pick(Choice choice);

    Choice m_choice = Choice::Cancel;
};
