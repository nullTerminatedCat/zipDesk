#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ArchiveService.hpp"

#include <bit7z/bit7z.hpp>
#include <bit7z/bitcompressionlevel.hpp>
#include <bit7z/biterror.hpp>
#include <bit7z/bitformat.hpp>
#include <bit7z/bitpropvariant.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

namespace zipdesk {
namespace {

namespace fs = std::filesystem;

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    }
    return out;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (size <= 0) {
        codePage = CP_ACP;
        size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    std::wstring out(size > 0 ? static_cast<std::size_t>(size) : 0, L'\0');
    if (size > 0) {
        MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    }
    return out;
}

bool isCancelled(const bit7z::BitException& error) {
    if (error.nativeCode() == E_ABORT) {
        return true;
    }
    if (error.code() == std::errc::operation_canceled) {
        return true;
    }
    const std::string text = error.what();
    return text.find("abort") != std::string::npos || text.find("Abort") != std::string::npos;
}

bool isPasswordError(const bit7z::BitException& error) {
    if (error.code() == bit7z::make_error_code(bit7z::BitError::InvalidZipPassword)) {
        return false;
    }
    return error.code() == bit7z::make_error_condition(bit7z::BitFailureSource::WrongPassword);
}

[[noreturn]] void rethrowBit(const bit7z::BitException& error) {
    if (isCancelled(error)) {
        throw ArchiveError(ErrorKind::Cancelled, L"已取消");
    }
    if (error.code() == bit7z::make_error_code(bit7z::BitError::InvalidZipPassword)) {
        throw ArchiveError(ErrorKind::InvalidPasswordCharset, L"ZIP 密码只能包含英文字符、数字和符号。");
    }
    if (isPasswordError(error)) {
        throw ArchiveError(ErrorKind::Password, L"密码不正确");
    }
    throw ArchiveError(ErrorKind::Other, L"操作失败：" + widen(error.what()));
}

void ensureZipPassword(ArchiveFormat format, const std::wstring& password) {
    if (format != ArchiveFormat::Zip || password.empty()) {
        return;
    }
    for (const wchar_t ch : password) {
        if (ch < 32 || ch >= 127) {
            throw ArchiveError(ErrorKind::InvalidPasswordCharset, L"ZIP 密码只能包含英文字符、数字和符号。");
        }
    }
}

const bit7z::BitInOutFormat& toFormat(ArchiveFormat format) {
    if (format == ArchiveFormat::Zip) {
        return bit7z::BitFormat::Zip;
    }
    if (format == ArchiveFormat::SevenZip) {
        return bit7z::BitFormat::SevenZip;
    }
    throw ArchiveError(ErrorKind::Other, L"不能创建 RAR，只能打开和解压。");
}

const bit7z::BitInFormat& toInFormat(ArchiveFormat format) {
    if (format == ArchiveFormat::Rar) {
        return bit7z::BitFormat::Auto;
    }
    return toFormat(format);
}

std::wstring findSfxModule(const std::wstring& dllPath) {
    const fs::path beside = fs::path(dllPath).parent_path() / L"7z.sfx";
    if (fs::exists(beside)) {
        return beside.wstring();
    }
    const fs::path installed(L"C:\\Program Files\\7-Zip\\7z.sfx");
    if (fs::exists(installed)) {
        return installed.wstring();
    }
    throw ArchiveError(ErrorKind::Other, L"找不到 7z.sfx，无法创建自解压程序。");
}

void writeSfx(const fs::path& sfx, const fs::path& payload, const fs::path& exePath) {
    std::ofstream output(exePath, std::ios::binary);
    std::ifstream stub(sfx, std::ios::binary);
    std::ifstream archive(payload, std::ios::binary);
    if (!output || !stub || !archive) {
        throw ArchiveError(ErrorKind::Other, L"无法写入自解压程序。");
    }
    output << stub.rdbuf();
    output << archive.rdbuf();
}

bit7z::BitCompressionLevel toLevel(CompressionLevel level) {
    switch (level) {
    case CompressionLevel::Store:
        return bit7z::BitCompressionLevel::None;
    case CompressionLevel::Fast:
        return bit7z::BitCompressionLevel::Fast;
    case CompressionLevel::Normal:
        return bit7z::BitCompressionLevel::Normal;
    case CompressionLevel::Maximum:
        return bit7z::BitCompressionLevel::Ultra;
    }
    return bit7z::BitCompressionLevel::Normal;
}

std::wstring formatTime(const bit7z::time_type& timePoint) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(timePoint);
    std::tm local{};
    if (localtime_s(&local, &raw) != 0) {
        return {};
    }
    wchar_t buffer[32] = {};
    if (std::wcsftime(buffer, 32, L"%Y-%m-%d %H:%M", &local) == 0) {
        return {};
    }
    return buffer;
}

bool included(const std::vector<std::uint32_t>& indices, std::uint32_t index) {
    if (indices.empty()) {
        return true;
    }
    return std::find(indices.begin(), indices.end(), index) != indices.end();
}

template <typename Handler>
void bindHooks(Handler& handler, const OperationHooks& hooks) {
    handler.setTotalCallback([&hooks](std::uint64_t total) {
        if (hooks.onTotal) {
            hooks.onTotal(total);
        }
    });
    handler.setProgressCallback([&hooks](std::uint64_t processed) {
        if (hooks.cancel != nullptr && hooks.cancel->load()) {
            return false;
        }
        if (hooks.onProgress) {
            hooks.onProgress(processed);
        }
        return hooks.cancel == nullptr || !hooks.cancel->load();
    });
    handler.setFileCallback([&hooks](bit7z::tstring file) {
        if (hooks.onFile) {
            hooks.onFile(file);
        }
    });
}

void removePath(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
}

void cleanupExtract(const ExtractRequest& request) {
    if (request.outputDir.empty()) {
        return;
    }
    const fs::path root(request.outputDir);
    if (request.removeOutputOnCancel) {
        removePath(root);
        return;
    }
    std::vector<fs::path> paths;
    paths.reserve(request.relativePaths.size());
    for (const std::wstring& relative : request.relativePaths) {
        if (!relative.empty()) {
            paths.push_back(root / relative);
        }
    }
    std::sort(paths.begin(), paths.end(), [](const fs::path& left, const fs::path& right) {
        return left.native().size() > right.native().size();
    });
    for (const fs::path& path : paths) {
        removePath(path);
    }
}

std::optional<std::wstring> askAgain(const OperationHooks& hooks, int attempt) {
    if (!hooks.askPassword) {
        return std::nullopt;
    }
    const wchar_t* prompt = attempt == 0 ? L"请输入密码。" : L"密码不正确，请再试一次。";
    return hooks.askPassword(prompt);
}

}  // namespace

class ArchiveService::LibraryHolder {
public:
    explicit LibraryHolder(const std::wstring& dllPath)
        : library(dllPath) {}

    bit7z::Bit7zLibrary library;
};

ArchiveError::ArchiveError(ErrorKind kind, std::wstring message)
    : m_kind(kind)
    , m_message(std::move(message))
    , m_what(narrow(m_message)) {}

const char* ArchiveError::what() const noexcept {
    return m_what.c_str();
}

ErrorKind ArchiveError::kind() const noexcept {
    return m_kind;
}

const std::wstring& ArchiveError::message() const noexcept {
    return m_message;
}

std::wstring normalizeArchivePath(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'\\') {
            ch = L'/';
        }
    }
    while (!path.empty() && (path.front() == L'/' || path.front() == L'.')) {
        if (path.front() == L'.' && path.size() > 1 && path[1] != L'/') {
            break;
        }
        if (path.front() == L'.' && path.size() == 1) {
            break;
        }
        path.erase(path.begin());
    }
    return path;
}

bool pathInside(const std::wstring& parent, const std::wstring& child) {
    if (parent.empty() || child.empty()) {
        return false;
    }
    if (child == parent) {
        return true;
    }
    const std::wstring prefix = parent.back() == L'/' ? parent : parent + L'/';
    return child.size() > prefix.size() && child.compare(0, prefix.size(), prefix) == 0;
}

bool isArchivePath(const std::wstring& path) {
    std::error_code error;
    if (path.empty() || fs::is_directory(path, error)) {
        return false;
    }
    std::wstring extension = fs::path(path).extension().wstring();
    for (wchar_t& ch : extension) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return extension == L".zip" || extension == L".7z" || extension == L".rar";
}

ArchiveFormat formatFromPath(const std::wstring& path) {
    std::wstring extension = fs::path(path).extension().wstring();
    for (wchar_t& ch : extension) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    if (extension == L".zip") {
        return ArchiveFormat::Zip;
    }
    if (extension == L".7z") {
        return ArchiveFormat::SevenZip;
    }
    if (extension == L".rar") {
        return ArchiveFormat::Rar;
    }
    throw ArchiveError(ErrorKind::Other, L"只支持 zip、7z 和 rar。");
}

std::vector<std::uint32_t> expandSelection(const std::vector<ArchiveEntry>& entries,
                                           const std::vector<std::uint32_t>& selected) {
    std::vector<const ArchiveEntry*> chosen;
    chosen.reserve(selected.size());
    for (const std::uint32_t index : selected) {
        for (const ArchiveEntry& entry : entries) {
            if (entry.index == index) {
                chosen.push_back(&entry);
                break;
            }
        }
    }

    std::vector<std::uint32_t> result;
    for (const ArchiveEntry& entry : entries) {
        bool take = false;
        for (const ArchiveEntry* item : chosen) {
            if (entry.index == item->index || (item->isDir && pathInside(item->path, entry.path))) {
                take = true;
                break;
            }
        }
        if (take) {
            result.push_back(entry.index);
        }
    }
    return result;
}

bool hasConflicts(const std::vector<ArchiveEntry>& entries,
                  const std::vector<std::uint32_t>& indices,
                  const std::wstring& outputDir) {
    if (outputDir.empty()) {
        return false;
    }
    const fs::path root(outputDir);
    for (const ArchiveEntry& entry : entries) {
        if (entry.isDir || entry.path.empty() || !included(indices, entry.index)) {
            continue;
        }
        std::error_code error;
        if (fs::exists(root / entry.path, error)) {
            return true;
        }
    }
    return false;
}

DropDecision decideDrop(const std::vector<std::wstring>& paths, bool archiveOpen) {
    DropDecision decision;
    std::vector<std::wstring> archives;
    std::vector<std::wstring> others;
    for (const std::wstring& path : paths) {
        if (path.empty()) {
            continue;
        }
        if (isArchivePath(path)) {
            archives.push_back(path);
        } else {
            others.push_back(path);
        }
    }
    if (!archives.empty()) {
        decision.action = DropAction::OpenArchive;
        decision.archivePath = archives.front();
        return decision;
    }
    decision.files = std::move(others);
    decision.action = archiveOpen ? DropAction::AppendFiles : DropAction::CreateArchive;
    return decision;
}

ArchiveService::~ArchiveService() = default;

ArchiveService::ArchiveService(std::wstring dllPath)
    : m_dllPath(std::move(dllPath)) {
    if (!fs::exists(m_dllPath)) {
        throw ArchiveError(ErrorKind::Other, L"找不到 7z.dll：" + m_dllPath);
    }
    try {
        m_library = std::make_unique<LibraryHolder>(m_dllPath);
    } catch (const bit7z::BitException& error) {
        rethrowBit(error);
    }
}

ArchiveInfo ArchiveService::read(const std::wstring& archivePath,
                                 std::wstring password,
                                 const OperationHooks& hooks) const {
    if (!fs::exists(archivePath)) {
        throw ArchiveError(ErrorKind::Other, L"找不到压缩包。");
    }
    const ArchiveFormat format = formatFromPath(archivePath);
    for (int attempt = 0; attempt < 5; ++attempt) {
        try {
            bit7z::BitArchiveReader reader{m_library->library, archivePath, toInFormat(format), password};
            ArchiveInfo info;
            info.path = archivePath;
            info.format = format;
            info.password = password;
            info.encrypted = reader.isEncrypted() || reader.hasEncryptedItems() || !password.empty();
            const auto properties = reader.archiveProperties();
            const auto comment = properties.find(bit7z::BitProperty::Comment);
            if (comment != properties.end() && comment->second.isString()) {
                info.comment = comment->second.getString();
            }
            const auto items = reader.items();
            info.entries.reserve(items.size());
            for (const auto& item : items) {
                ArchiveEntry entry;
                entry.index = item.index();
                entry.path = normalizeArchivePath(item.path().empty() ? item.name() : item.path());
                entry.name = item.name().empty() ? entry.path : item.name();
                entry.size = item.size();
                entry.packSize = item.packSize();
                entry.isDir = item.isDir();
                entry.encrypted = item.isEncrypted();
                entry.isLink = item.isSymLink();
                entry.crc = item.crc();
                entry.hasCrc = !entry.isDir;
                const bit7z::BitPropVariant modified = item.itemProperty(bit7z::BitProperty::MTime);
                if (modified.isFileTime()) {
                    entry.hasModified = true;
                    entry.modified = formatTime(modified.getTimePoint());
                }
                info.entries.push_back(std::move(entry));
            }
            return info;
        } catch (const bit7z::BitException& error) {
            ArchiveError translated(ErrorKind::Other, L"");
            try {
                rethrowBit(error);
            } catch (const ArchiveError& caught) {
                translated = caught;
            }
            if (translated.kind() != ErrorKind::Password || !hooks.askPassword) {
                throw translated;
            }
            const auto next = askAgain(hooks, attempt);
            if (!next) {
                throw ArchiveError(ErrorKind::Cancelled, L"已取消");
            }
            password = *next;
        }
    }
    throw ArchiveError(ErrorKind::Password, L"密码不正确");
}

void ArchiveService::extract(const ExtractRequest& request, const OperationHooks& hooks) const {
    if (!fs::exists(request.archivePath)) {
        throw ArchiveError(ErrorKind::Other, L"找不到压缩包。");
    }
    ExtractRequest current = request;
    for (int attempt = 0; attempt < 5; ++attempt) {
        try {
            const ArchiveFormat format = formatFromPath(current.archivePath);
            fs::create_directories(current.outputDir);
            bit7z::BitFileExtractor extractor{m_library->library, toInFormat(format)};
            extractor.setOverwriteMode(current.overwrite == OverwritePolicy::Skip ? bit7z::OverwriteMode::Skip
                                                                                   : bit7z::OverwriteMode::Overwrite);
            if (!current.password.empty()) {
                extractor.setPassword(current.password);
            }
            bindHooks(extractor, hooks);
            if (current.indices.empty()) {
                extractor.extract(current.archivePath, current.outputDir);
            } else {
                extractor.extractItems(current.archivePath, current.indices, current.outputDir);
            }
            return;
        } catch (const bit7z::BitException& error) {
            ArchiveError translated(ErrorKind::Other, L"");
            try {
                rethrowBit(error);
            } catch (const ArchiveError& caught) {
                translated = caught;
            }
            if (translated.kind() == ErrorKind::Cancelled) {
                cleanupExtract(current);
                throw translated;
            }
            if (translated.kind() != ErrorKind::Password || !hooks.askPassword) {
                throw translated;
            }
            const auto next = askAgain(hooks, attempt);
            if (!next) {
                throw ArchiveError(ErrorKind::Cancelled, L"已取消");
            }
            current.password = *next;
        }
    }
    throw ArchiveError(ErrorKind::Password, L"密码不正确");
}

void ArchiveService::compress(const CompressRequest& request, const OperationHooks& hooks) const {
    if (request.inputs.empty()) {
        throw ArchiveError(ErrorKind::Other, L"没有要压缩的文件。");
    }
    for (const std::wstring& input : request.inputs) {
        if (!fs::exists(input)) {
            throw ArchiveError(ErrorKind::Other, L"找不到要压缩的文件：" + input);
        }
    }
    if (request.format == ArchiveFormat::Rar) {
        throw ArchiveError(ErrorKind::Other, L"不能创建 RAR，只能打开和解压。");
    }
    if (request.sfx && request.format != ArchiveFormat::SevenZip) {
        throw ArchiveError(ErrorKind::Other, L"自解压程序只支持 7z。");
    }
    ensureZipPassword(request.format, request.password);
    const fs::path output(request.outputPath);
    if (output.has_parent_path()) {
        fs::create_directories(output.parent_path());
    }
    const fs::path payload = request.sfx ? fs::path(output.wstring() + L".payload") : output;
    const bool existed = fs::exists(output);
    try {
        bit7z::BitFileCompressor compressor{m_library->library, toFormat(request.format)};
        compressor.setCompressionLevel(toLevel(request.level));
        compressor.setOverwriteMode(bit7z::OverwriteMode::Overwrite);
        compressor.setSolidMode(request.solid && request.format == ArchiveFormat::SevenZip);
        if (request.volumeSize > 0 && !request.sfx) {
            compressor.setVolumeSize(request.volumeSize);
        }
        if (!request.password.empty()) {
            compressor.setPassword(request.password);
        }
        bindHooks(compressor, hooks);
        compressor.compress(request.inputs, payload.wstring());
        if (request.sfx) {
            writeSfx(findSfxModule(m_dllPath), payload, output);
            removePath(payload);
        }
    } catch (const bit7z::BitException& error) {
        ArchiveError translated(ErrorKind::Other, L"");
        try {
            rethrowBit(error);
        } catch (const ArchiveError& caught) {
            translated = caught;
        }
        if (translated.kind() == ErrorKind::Cancelled && !existed) {
            removePath(output);
        }
        throw translated;
    }
}

void ArchiveService::append(const AppendRequest& request, const OperationHooks& hooks) const {
    if (formatFromPath(request.archivePath) == ArchiveFormat::Rar) {
        throw ArchiveError(ErrorKind::Other, L"不能修改 RAR，只能打开和解压。");
    }
    if (!fs::exists(request.archivePath)) {
        throw ArchiveError(ErrorKind::Other, L"找不到压缩包。");
    }
    if (request.inputs.empty()) {
        throw ArchiveError(ErrorKind::Other, L"没有要追加的文件。");
    }
    for (const std::wstring& input : request.inputs) {
        if (!fs::exists(input)) {
            throw ArchiveError(ErrorKind::Other, L"找不到要追加的文件：" + input);
        }
    }
    const ArchiveFormat format = formatFromPath(request.archivePath);
    ensureZipPassword(format, request.password);
    std::wstring password = request.password;
    for (int attempt = 0; attempt < 5; ++attempt) {
        try {
            bit7z::BitArchiveEditor editor{m_library->library, request.archivePath, toFormat(format), password};
            editor.setUpdateMode(bit7z::UpdateMode::Update);
            editor.setCompressionLevel(toLevel(request.level));
            if (!password.empty()) {
                editor.setPassword(password);
            }
            bindHooks(editor, hooks);
            for (const std::wstring& input : request.inputs) {
                std::error_code error;
                if (fs::is_directory(input, error)) {
                    editor.addDirectory(input);
                } else {
                    editor.addFile(input);
                }
            }
            editor.applyChanges();
            return;
        } catch (const bit7z::BitException& error) {
            ArchiveError translated(ErrorKind::Other, L"");
            try {
                rethrowBit(error);
            } catch (const ArchiveError& caught) {
                translated = caught;
            }
            if (translated.kind() != ErrorKind::Password || !hooks.askPassword) {
                throw translated;
            }
            const auto next = askAgain(hooks, attempt);
            if (!next) {
                throw ArchiveError(ErrorKind::Cancelled, L"已取消");
            }
            password = *next;
            ensureZipPassword(format, password);
        }
    }
    throw ArchiveError(ErrorKind::Password, L"密码不正确");
}

std::vector<std::uint8_t> ArchiveService::readItem(const std::wstring& archivePath,
                                                   std::uint32_t index,
                                                   std::wstring password,
                                                   const OperationHooks& hooks) const {
    const ArchiveFormat format = formatFromPath(archivePath);
    for (int attempt = 0; attempt < 5; ++attempt) {
        try {
            bit7z::BitFileExtractor extractor{m_library->library, toInFormat(format)};
            if (!password.empty()) {
                extractor.setPassword(password);
            }
            bindHooks(extractor, hooks);
            std::vector<bit7z::byte_t> buffer;
            extractor.extract(archivePath, buffer, index);
            return std::vector<std::uint8_t>(buffer.begin(), buffer.end());
        } catch (const bit7z::BitException& error) {
            ArchiveError translated(ErrorKind::Other, L"");
            try {
                rethrowBit(error);
            } catch (const ArchiveError& caught) {
                translated = caught;
            }
            if (translated.kind() != ErrorKind::Password || !hooks.askPassword) {
                throw translated;
            }
            const auto next = askAgain(hooks, attempt);
            if (!next) {
                throw ArchiveError(ErrorKind::Cancelled, L"已取消");
            }
            password = *next;
        }
    }
    throw ArchiveError(ErrorKind::Password, L"密码不正确");
}

ArchiveService::RepairResult ArchiveService::repair(const std::wstring& archivePath,
                                                    std::wstring password,
                                                    const OperationHooks& hooks) const {
    const ArchiveInfo info = read(archivePath, password, hooks);
    const fs::path archive(archivePath);
    RepairResult result;
    result.outputDir = (archive.parent_path() / (archive.stem().wstring() + L"_repaired")).wstring();
    fs::create_directories(result.outputDir);
    for (const ArchiveEntry& entry : info.entries) {
        if (entry.isDir) {
            continue;
        }
        try {
            ExtractRequest request;
            request.archivePath = archivePath;
            request.outputDir = result.outputDir;
            request.password = info.password.empty() ? password : info.password;
            request.indices = {entry.index};
            request.relativePaths = {entry.path};
            request.overwrite = OverwritePolicy::Overwrite;
            extract(request, hooks);
            ++result.recovered;
        } catch (const ArchiveError&) {
            ++result.failed;
        }
    }
    return result;
}

namespace {

template <typename Operation>
void editArchive(const bit7z::Bit7zLibrary& library,
                 const std::wstring& archivePath,
                 std::wstring password,
                 const OperationHooks& hooks,
                 Operation operation) {
    const ArchiveFormat format = formatFromPath(archivePath);
    if (format == ArchiveFormat::Rar) {
        throw ArchiveError(ErrorKind::Other, L"不能修改 RAR，只能打开和解压。");
    }
    ensureZipPassword(format, password);
    for (int attempt = 0; attempt < 5; ++attempt) {
        try {
            bit7z::BitArchiveEditor editor{library, archivePath, toFormat(format), password};
            editor.setUpdateMode(bit7z::UpdateMode::Update);
            if (!password.empty()) {
                editor.setPassword(password);
            }
            bindHooks(editor, hooks);
            operation(editor);
            editor.applyChanges();
            return;
        } catch (const bit7z::BitException& error) {
            ArchiveError translated(ErrorKind::Other, L"");
            try {
                rethrowBit(error);
            } catch (const ArchiveError& caught) {
                translated = caught;
            }
            if (translated.kind() != ErrorKind::Password || !hooks.askPassword) {
                throw translated;
            }
            const auto next = askAgain(hooks, attempt);
            if (!next) {
                throw ArchiveError(ErrorKind::Cancelled, L"已取消");
            }
            password = *next;
            ensureZipPassword(format, password);
        }
    }
    throw ArchiveError(ErrorKind::Password, L"密码不正确");
}

}  // namespace

void ArchiveService::test(const std::wstring& archivePath, std::wstring password, const OperationHooks& hooks) const {
    if (!fs::exists(archivePath)) {
        throw ArchiveError(ErrorKind::Other, L"找不到压缩包。");
    }
    const ArchiveFormat format = formatFromPath(archivePath);
    for (int attempt = 0; attempt < 5; ++attempt) {
        try {
            bit7z::BitFileExtractor extractor{m_library->library, toInFormat(format)};
            if (!password.empty()) {
                extractor.setPassword(password);
            }
            bindHooks(extractor, hooks);
            extractor.test(archivePath);
            return;
        } catch (const bit7z::BitException& error) {
            ArchiveError translated(ErrorKind::Other, L"");
            try {
                rethrowBit(error);
            } catch (const ArchiveError& caught) {
                translated = caught;
            }
            if (translated.kind() != ErrorKind::Password || !hooks.askPassword) {
                throw translated;
            }
            const auto next = askAgain(hooks, attempt);
            if (!next) {
                throw ArchiveError(ErrorKind::Cancelled, L"已取消");
            }
            password = *next;
        }
    }
    throw ArchiveError(ErrorKind::Password, L"密码不正确");
}

void ArchiveService::removeItems(const std::wstring& archivePath,
                                 std::vector<std::uint32_t> indices,
                                 std::wstring password,
                                 const OperationHooks& hooks) const {
    if (indices.empty()) {
        throw ArchiveError(ErrorKind::Other, L"没有要删除的项目。");
    }
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    editArchive(m_library->library, archivePath, std::move(password), hooks, [&](bit7z::BitArchiveEditor& editor) {
        for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
            editor.deleteItem(*it, bit7z::DeletePolicy::RecurseDirs);
        }
    });
}

void ArchiveService::renameItem(const std::wstring& archivePath,
                                std::uint32_t index,
                                const std::wstring& newPath,
                                std::wstring password,
                                const OperationHooks& hooks) const {
    if (newPath.empty()) {
        throw ArchiveError(ErrorKind::Other, L"名称不能为空。");
    }
    editArchive(m_library->library, archivePath, std::move(password), hooks, [&](bit7z::BitArchiveEditor& editor) {
        editor.renameItem(index, newPath);
    });
}

void ArchiveService::replaceItem(const std::wstring& archivePath,
                                 std::uint32_t index,
                                 const std::wstring& filePath,
                                 std::wstring password,
                                 const OperationHooks& hooks) const {
    editArchive(m_library->library, archivePath, std::move(password), hooks, [&](bit7z::BitArchiveEditor& editor) {
        editor.updateItem(index, filePath);
    });
}

}  // namespace zipdesk
