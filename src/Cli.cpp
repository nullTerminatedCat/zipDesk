#include "Cli.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace zipdesk {
namespace {

void prepareOutput() {
    const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing != nullptr && existing != INVALID_HANDLE_VALUE) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
}

void writeOut(const std::wstring& text) {
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    const std::wstring line = text + L"\r\n";
    DWORD mode = 0;
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(handle, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        return;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes > 1 ? static_cast<size_t>(bytes - 1) : 0, '\0');
    if (bytes > 1) {
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes - 1, nullptr, nullptr);
    }
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

std::wstring lower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return text;
}

std::uint64_t parseSize(const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }
    wchar_t suffix = text.back();
    std::wstring number = text;
    std::uint64_t scale = 1;
    if (suffix == L'k' || suffix == L'K' || suffix == L'm' || suffix == L'M' || suffix == L'g' || suffix == L'G') {
        number.pop_back();
        if (suffix == L'k' || suffix == L'K') {
            scale = 1024;
        } else if (suffix == L'm' || suffix == L'M') {
            scale = 1024ull * 1024;
        } else {
            scale = 1024ull * 1024 * 1024;
        }
    }
    return static_cast<std::uint64_t>(std::stoull(number)) * scale;
}

struct CliRequest {
    std::wstring command;
    std::wstring archive;
    std::vector<std::wstring> files;
    std::wstring output;
    std::wstring password;
    std::uint64_t volume = 0;
    bool solid = false;
    bool sfx = false;
    bool yes = false;
};

CliRequest parseCli(const std::vector<std::wstring>& args) {
    CliRequest request;
    request.command = lower(args[1]);
    for (size_t i = 2; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        if (arg.rfind(L"-o", 0) == 0 || arg.rfind(L"-O", 0) == 0) {
            request.output = arg.size() == 2 && i + 1 < args.size() ? args[++i] : arg.substr(2);
        } else if (arg.rfind(L"-p", 0) == 0 || arg.rfind(L"-P", 0) == 0) {
            request.password = arg.size() == 2 && i + 1 < args.size() ? args[++i] : arg.substr(2);
        } else if (arg.rfind(L"-v", 0) == 0 || arg.rfind(L"-V", 0) == 0) {
            const std::wstring size = arg.size() == 2 && i + 1 < args.size() ? args[++i] : arg.substr(2);
            request.volume = parseSize(size);
        } else if (lower(arg).rfind(L"-ms", 0) == 0) {
            request.solid = lower(arg).find(L"off") == std::wstring::npos;
        } else if (lower(arg) == L"-sfx") {
            request.sfx = true;
        } else if (arg == L"-y" || arg == L"-Y") {
            request.yes = true;
        } else if (request.archive.empty()) {
            request.archive = arg;
        } else {
            request.files.push_back(arg);
        }
    }
    return request;
}

int fail(const std::wstring& message) {
    writeOut(message);
    return 1;
}

}  // namespace

bool isCliInvocation(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        return false;
    }
    const std::wstring command = lower(args[1]);
    return command == L"a" || command == L"d" || command == L"e" || command == L"l" || command == L"rn" ||
           command == L"t" || command == L"u" || command == L"x";
}

int runCli(ArchiveService& service, const std::vector<std::wstring>& args) {
    prepareOutput();
    const CliRequest request = parseCli(args);
    OperationHooks hooks;
    try {
        if (request.command == L"l") {
            const ArchiveInfo info = service.read(request.archive, request.password, hooks);
            for (const ArchiveEntry& entry : info.entries) {
                writeOut(entry.path + L"\t" + std::to_wstring(entry.size) + L"\t" + std::to_wstring(entry.packSize));
            }
            return 0;
        }
        if (request.command == L"t") {
            service.test(request.archive, request.password, hooks);
            writeOut(L"OK");
            return 0;
        }
        if (request.command == L"x" || request.command == L"e") {
            ExtractRequest extract;
            extract.archivePath = request.archive;
            extract.password = request.password;
            extract.outputDir = request.output.empty() ? std::filesystem::current_path().wstring() : request.output;
            service.extract(extract, hooks);
            writeOut(L"OK");
            return 0;
        }
        if (request.command == L"a" || request.command == L"u") {
            if (request.command == L"u" && std::filesystem::exists(request.archive)) {
                AppendRequest append;
                append.archivePath = request.archive;
                append.inputs = request.files;
                append.password = request.password;
                service.append(append, hooks);
            } else {
                CompressRequest compress;
                compress.inputs = request.files;
                compress.outputPath = request.archive;
                compress.password = request.password;
                compress.volumeSize = request.volume;
                compress.solid = request.solid;
                compress.sfx = request.sfx;
                compress.format = formatFromPath(request.sfx ? L"a.7z" : request.archive);
                service.compress(compress, hooks);
            }
            writeOut(L"OK");
            return 0;
        }
        if (request.command == L"d") {
            const ArchiveInfo info = service.read(request.archive, request.password, hooks);
            std::vector<std::uint32_t> indices;
            for (const ArchiveEntry& entry : info.entries) {
                for (const std::wstring& file : request.files) {
                    if (entry.path == file || entry.name == file) {
                        indices.push_back(entry.index);
                    }
                }
            }
            service.removeItems(request.archive, indices, request.password, hooks);
            writeOut(L"OK");
            return 0;
        }
        if (request.command == L"rn") {
            if (request.files.size() < 2) {
                return fail(L"rn needs old and new names");
            }
            const ArchiveInfo info = service.read(request.archive, request.password, hooks);
            for (const ArchiveEntry& entry : info.entries) {
                if (entry.path == request.files[0] || entry.name == request.files[0]) {
                    service.renameItem(request.archive, entry.index, request.files[1], request.password, hooks);
                    writeOut(L"OK");
                    return 0;
                }
            }
            return fail(L"item not found");
        }
    } catch (const ArchiveError& error) {
        return fail(error.message());
    } catch (const std::exception& error) {
        writeOut(std::wstring(error.what(), error.what() + std::strlen(error.what())));
        return 1;
    }
    return fail(L"unsupported command");
}

}  // namespace zipdesk
