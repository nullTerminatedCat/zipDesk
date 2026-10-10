#include "Launch.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace zipdesk {
namespace {

bool isOption(const std::wstring& text, const wchar_t* name) {
    return text == name;
}

}  // namespace

LaunchCommand parseLaunch(const std::vector<std::wstring>& args) {
    LaunchCommand command;
    if (args.size() < 2) {
        return command;
    }

    const std::wstring& first = args[1];
    LaunchAction action = LaunchAction::None;
    std::size_t pathIndex = 1;
    if (isOption(first, L"--extract-here")) {
        action = LaunchAction::ExtractHere;
        pathIndex = 2;
    } else if (isOption(first, L"--extract-to-folder")) {
        action = LaunchAction::ExtractToFolder;
        pathIndex = 2;
    } else if (isOption(first, L"--compress-zip")) {
        action = LaunchAction::CompressZip;
        pathIndex = 2;
    } else if (isOption(first, L"--compress-7z")) {
        action = LaunchAction::Compress7z;
        pathIndex = 2;
    } else if (isOption(first, L"--register")) {
        command.action = LaunchAction::Register;
        return command;
    } else if (isOption(first, L"--unregister")) {
        command.action = LaunchAction::Unregister;
        return command;
    } else if (!first.empty() && first[0] != L'-') {
        action = LaunchAction::Open;
        pathIndex = 1;
    }

    command.action = action;
    for (std::size_t index = pathIndex; index < args.size(); ++index) {
        if (!args[index].empty()) {
            command.paths.push_back(args[index]);
        }
    }
    if (action != LaunchAction::None && action != LaunchAction::Open && command.paths.empty()) {
        command.action = LaunchAction::None;
    }
    if (action == LaunchAction::Open && command.paths.empty()) {
        command.action = LaunchAction::None;
    }
    return command;
}

namespace {

std::filesystem::path batchFilePath() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path dir = std::filesystem::path(temp) / L"ZipDesk";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return dir / L"shell-batch.txt";
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes > 0 ? static_cast<size_t>(bytes) : 0, '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
    }
    return utf8;
}

std::wstring fromUtf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(chars > 0 ? static_cast<size_t>(chars) : 0, L'\0');
    if (chars > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), chars);
    }
    return wide;
}

std::wstring batchFormat(LaunchAction action) {
    return action == LaunchAction::CompressZip ? L"zip" : L"7z";
}

bool fileIsRecent(const std::filesystem::path& path, unsigned maxAgeMs) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        return false;
    }
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER current{now.dwLowDateTime, now.dwHighDateTime};
    ULARGE_INTEGER written{info.ftLastWriteTime.dwLowDateTime, info.ftLastWriteTime.dwHighDateTime};
    if (current.QuadPart < written.QuadPart) {
        return true;
    }
    return (current.QuadPart - written.QuadPart) / 10000ULL <= maxAgeMs;
}

std::vector<std::wstring> readBatch(const std::filesystem::path& path, std::wstring& format) {
    std::vector<std::wstring> paths;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!std::getline(input, line)) {
        return paths;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    format = fromUtf8(line);
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            paths.push_back(fromUtf8(line));
        }
    }
    return paths;
}

void writeBatch(const std::filesystem::path& path, const std::wstring& format, const std::vector<std::wstring>& paths) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << toUtf8(format) << "\n";
    for (const std::wstring& item : paths) {
        output << toUtf8(item) << "\n";
    }
}

void addUnique(std::vector<std::wstring>& paths, const std::wstring& path) {
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(path);
    }
}

class BatchMutex {
public:
    BatchMutex() {
        handle = CreateMutexW(nullptr, FALSE, L"Local\\ZipDeskShellCompress");
        if (handle != nullptr) {
            WaitForSingleObject(handle, 3000);
        }
    }
    ~BatchMutex() {
        if (handle != nullptr) {
            ReleaseMutex(handle);
            CloseHandle(handle);
        }
    }
    BatchMutex(const BatchMutex&) = delete;
    BatchMutex& operator=(const BatchMutex&) = delete;

private:
    HANDLE handle = nullptr;
};

}  // namespace

bool claimShellBatch(LaunchCommand& command) {
    if (command.action != LaunchAction::CompressZip && command.action != LaunchAction::Compress7z) {
        return true;
    }
    if (command.paths.empty()) {
        return true;
    }

    const std::filesystem::path file = batchFilePath();
    const std::wstring format = batchFormat(command.action);
    bool leader = false;
    {
        const BatchMutex lock;
        std::wstring existingFormat;
        const bool join = fileIsRecent(file, 2500) && !readBatch(file, existingFormat).empty() && existingFormat == format;
        if (join) {
            std::wstring ignored;
            std::vector<std::wstring> paths = readBatch(file, ignored);
            for (const std::wstring& path : command.paths) {
                addUnique(paths, path);
            }
            writeBatch(file, format, paths);
        } else {
            writeBatch(file, format, command.paths);
            leader = true;
        }
    }
    if (!leader) {
        return false;
    }

    std::size_t seen = command.paths.size();
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 1400) {
        Sleep(160);
        std::vector<std::wstring> paths;
        {
            const BatchMutex lock;
            std::wstring ignored;
            paths = readBatch(file, ignored);
        }
        if (paths.size() == seen) {
            Sleep(160);
            std::vector<std::wstring> again;
            {
                const BatchMutex lock;
                std::wstring ignored;
                again = readBatch(file, ignored);
            }
            if (again.size() == seen && !again.empty()) {
                command.paths = std::move(again);
                const BatchMutex lock;
                std::error_code error;
                std::filesystem::remove(file, error);
                return true;
            }
            seen = again.size();
        } else {
            seen = paths.size();
        }
    }

    const BatchMutex lock;
    std::wstring ignored;
    const std::vector<std::wstring> gathered = readBatch(file, ignored);
    if (!gathered.empty()) {
        command.paths = gathered;
    }
    std::error_code error;
    std::filesystem::remove(file, error);
    return true;
}

}  // namespace zipdesk
