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
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
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

[[noreturn]] void rethrowBit(const bit7z::BitException& error, bool cancelRequested = false) {
    if (cancelRequested && isCancelled(error)) {
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
    output.flush();
    if (!output || stub.bad() || archive.bad()) {
        throw ArchiveError(ErrorKind::Other, L"无法完整写入自解压程序。");
    }
}

fs::path stagingPath(const fs::path& target) {
    static std::atomic<std::uint64_t> sequence{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::wstring suffix = L".zipdesk-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                    std::to_wstring(GetTickCount64()) + L"-" +
                                    std::to_wstring(sequence.fetch_add(1));
        const fs::path candidate = target.parent_path() /
            (target.stem().wstring() + suffix + target.extension().wstring());
        std::error_code error;
        if (!fs::exists(candidate, error) && !error) {
            return candidate;
        }
    }
    throw ArchiveError(ErrorKind::Other, L"无法创建临时压缩包文件名。");
}

void commitStagedFile(const fs::path& staged, const fs::path& target) {
    if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::error_code error(static_cast<int>(GetLastError()), std::system_category());
        throw ArchiveError(ErrorKind::Other, L"无法保存压缩包：" + widen(error.message()));
    }
}

void removePath(const fs::path& path);

std::vector<fs::path> volumeFiles(const fs::path& prefix) {
    std::vector<fs::path> files;
    const fs::path directory = prefix.has_parent_path() ? prefix.parent_path() : fs::current_path();
    const std::wstring name = prefix.filename().wstring() + L".";
    for (const auto& entry : fs::directory_iterator(directory)) {
        const std::wstring candidate = entry.path().filename().wstring();
        if (candidate.size() <= name.size() || candidate.compare(0, name.size(), name) != 0) continue;
        const std::wstring suffix = candidate.substr(name.size());
        if (std::all_of(suffix.begin(), suffix.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) {
            if (!entry.is_regular_file()) {
                throw ArchiveError(ErrorKind::Other, L"分卷路径不是普通文件：" + entry.path().wstring());
            }
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

void removeStagedVolumes(const fs::path& staged) {
    for (const fs::path& volume : volumeFiles(staged)) {
        removePath(volume);
    }
}

void moveWithoutCopy(const fs::path& source, const fs::path& target, bool replace = false) {
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(source.c_str(), target.c_str(), flags)) {
        const std::error_code error(static_cast<int>(GetLastError()), std::system_category());
        throw ArchiveError(ErrorKind::Other, L"无法移动分卷文件：" + widen(error.message()));
    }
}

void commitStagedVolumes(const fs::path& staged, const fs::path& target) {
    const auto fresh = volumeFiles(staged);
    if (fresh.empty()) {
        throw ArchiveError(ErrorKind::Other, L"没有生成分卷文件。");
    }
    std::vector<std::pair<fs::path, fs::path>> saved;
    std::vector<fs::path> installed;
    try {
        auto old = volumeFiles(target);
        if (fs::exists(target)) old.push_back(target);
        for (const fs::path& path : old) {
            const fs::path backup(path.wstring() + L".zipdesk-backup-" + staged.filename().wstring());
            moveWithoutCopy(path, backup);
            saved.emplace_back(path, backup);
        }
        for (const fs::path& path : fresh) {
            const std::wstring suffix = path.wstring().substr(staged.wstring().size());
            const fs::path destination(target.wstring() + suffix);
            moveWithoutCopy(path, destination);
            installed.push_back(destination);
        }
    } catch (...) {
        for (const fs::path& path : installed) removePath(path);
        bool restored = true;
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (!MoveFileExW(it->second.c_str(), it->first.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) restored = false;
        }
        if (!restored) {
            throw ArchiveError(ErrorKind::Other, L"分卷提交失败，旧分卷的备份仍留在原目录，请检查后恢复。");
        }
        throw;
    }
    for (const auto& item : saved) removePath(item.second);
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
    if (request.outputDir.empty() || !request.removeOutputOnCancel) {
        return;
    }
    const fs::path root(request.outputDir);
    std::error_code error;
    fs::remove(root, error);  // Preserve files added during extraction.
}

void appendResult(ExtractResult& into, const ExtractResult& from) {
    into.written += from.written;
    into.overwritten += from.overwritten;
    into.keptCopies.insert(into.keptCopies.end(), from.keptCopies.begin(), from.keptCopies.end());
    into.skipped.insert(into.skipped.end(), from.skipped.begin(), from.skipped.end());
    into.missing.insert(into.missing.end(), from.missing.begin(), from.missing.end());
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

SmartExtractDestination smartExtractDestination(const std::wstring& archivePath,
                                               const std::vector<ArchiveEntry>& entries) {
    if (entries.empty()) {
        throw ArchiveError(ErrorKind::Other, L"这个压缩包是空的，没有需要解压的文件。");
    }
    const fs::path archive = fs::absolute(archivePath);
    const fs::path parent = archive.parent_path();
    std::wstring root;
    bool singleDirectory = true;
    for (const auto& entry : entries) {
        std::wstring path = entry.path;
        std::replace(path.begin(), path.end(), L'\\', L'/');
        if (entry.isDir && !path.empty() && path.back() == L'/') path.pop_back();
        if (path.empty() || path.front() == L'/' || path.find(L':') != std::wstring::npos) {
            throw ArchiveError(ErrorKind::Other, L"压缩包包含异常路径，无法智能解压。");
        }
        std::size_t start = 0;
        while (start < path.size()) {
            const auto end = path.find(L'/', start);
            const auto part = path.substr(start, end == std::wstring::npos ? end : end - start);
            if (part.empty() || part == L"." || part == L".." ||
                part.back() == L'.' || part.back() == L' ') {
                throw ArchiveError(ErrorKind::Other, L"压缩包包含异常路径，无法智能解压。");
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        const auto slash = path.find(L'/');
        const auto top = path.substr(0, slash);
        if (root.empty()) root = top;
        if (root != top || entry.isLink || (slash == std::wstring::npos && !entry.isDir))
            singleDirectory = false;
    }
    if (singleDirectory) return {parent.wstring(), (parent / root).wstring()};
    auto name = archive.stem().wstring();
    if (name.empty() || name == L"." || name == L"..") name = L"extracted";
    const auto folder = (parent / name).wstring();
    return {folder, folder};
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


ContentFormat sniffContentFormat(const std::wstring& path) {
    std::ifstream input(fs::path(path), std::ios::binary);
    if (!input) {
        return ContentFormat::Unknown;
    }
    std::vector<unsigned char> header(0x9008);
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, input.gcount())));
    const auto got = header.size();
    auto at = [&](std::size_t offset, const char* text, std::size_t n) {
        if (got < offset + n) return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (header[offset + i] != static_cast<unsigned char>(text[i])) return false;
        }
        return true;
    };
    if (got >= 4 && header[0] == 'P' && header[1] == 'K' &&
        ((header[2] == 3 && header[3] == 4) || (header[2] == 5 && header[3] == 6) ||
         (header[2] == 7 && header[3] == 8))) {
        return ContentFormat::Zip;
    }
    if (got >= 6 && header[0] == '7' && header[1] == 'z' && header[2] == 0xBC && header[3] == 0xAF &&
        header[4] == 0x27 && header[5] == 0x1C) {
        return ContentFormat::SevenZip;
    }
    if (got >= 7 && header[0] == 'R' && header[1] == 'a' && header[2] == 'r' && header[3] == '!' &&
        header[4] == 0x1A && header[5] == 0x07) {
        return ContentFormat::Rar;
    }
    if (got >= 2 && header[0] == 0x1F && header[1] == 0x8B) {
        return ContentFormat::GZip;
    }
    if (got >= 6 && header[0] == 0xFD && header[1] == '7' && header[2] == 'z' && header[3] == 'X' &&
        header[4] == 'Z' && header[5] == 0x00) {
        return ContentFormat::Xz;
    }
    if (at(257, "ustar", 5)) {
        return ContentFormat::Tar;
    }
    if (at(0x8001, "CD001", 5) || at(0x8801, "CD001", 5) || at(0x9001, "CD001", 5)) {
        return ContentFormat::Iso;
    }
    return ContentFormat::Unknown;
}

std::wstring contentFormatName(ContentFormat format) {
    switch (format) {
    case ContentFormat::Zip:
        return L"ZIP";
    case ContentFormat::SevenZip:
        return L"7z";
    case ContentFormat::Rar:
        return L"RAR";
    case ContentFormat::Tar:
        return L"TAR";
    case ContentFormat::GZip:
        return L"GZIP";
    case ContentFormat::Xz:
        return L"XZ";
    case ContentFormat::Iso:
        return L"ISO";
    case ContentFormat::Unknown:
    default:
        return L"未知";
    }
}

namespace {

std::wstring lowerExtension(const std::wstring& path) {
    std::wstring extension = fs::path(path).extension().wstring();
    for (wchar_t& ch : extension) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    // .tar.gz / .tar.xz style: path.extension() is only the last suffix.
    const std::wstring name = fs::path(path).filename().wstring();
    std::wstring lowered = name;
    for (wchar_t& ch : lowered) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    if (lowered.size() > 7) {
        const auto tail7 = lowered.substr(lowered.size() - 7);
        if (tail7 == L".tar.gz") return L".tar.gz";
        if (tail7 == L".tar.xz") return L".tar.xz";
    }
    if (lowered.size() > 8 && lowered.substr(lowered.size() - 8) == L".tar.bz2") {
        return L".tar.bz2";
    }
    return extension;
}

std::optional<ArchiveFormat> formatFromExtension(const std::wstring& path) {
    const std::wstring extension = lowerExtension(path);
    if (extension == L".zip") {
        return ArchiveFormat::Zip;
    }
    if (extension == L".7z") {
        return ArchiveFormat::SevenZip;
    }
    if (extension == L".rar") {
        return ArchiveFormat::Rar;
    }
    return std::nullopt;
}

std::optional<ArchiveFormat> readableFromContent(ContentFormat sniffed) {
    switch (sniffed) {
    case ContentFormat::Zip:
        return ArchiveFormat::Zip;
    case ContentFormat::SevenZip:
        return ArchiveFormat::SevenZip;
    case ContentFormat::Rar:
        return ArchiveFormat::Rar;
    default:
        return std::nullopt;
    }
}

bool isRecognizedUnsupported(ContentFormat sniffed) {
    return sniffed == ContentFormat::Tar || sniffed == ContentFormat::GZip || sniffed == ContentFormat::Xz ||
           sniffed == ContentFormat::Iso;
}

ContentFormat unsupportedFromExtension(const std::wstring& path) {
    const std::wstring extension = lowerExtension(path);
    if (extension == L".tar") return ContentFormat::Tar;
    if (extension == L".gz" || extension == L".tgz" || extension == L".tar.gz") return ContentFormat::GZip;
    if (extension == L".xz" || extension == L".txz" || extension == L".tar.xz") return ContentFormat::Xz;
    if (extension == L".iso") return ContentFormat::Iso;
    if (extension == L".zipx") return ContentFormat::Zip;  // treated specially by caller
    return ContentFormat::Unknown;
}

std::wstring archiveFormatName(ArchiveFormat format) {
    switch (format) {
    case ArchiveFormat::Zip:
        return L"ZIP";
    case ArchiveFormat::SevenZip:
        return L"7z";
    case ArchiveFormat::Rar:
        return L"RAR";
    }
    return L"未知";
}

[[noreturn]] void throwUnsupported(ContentFormat sniffed, const std::optional<ArchiveFormat>& byExt) {
    std::wstring message = L"识别到文件内容为 " + contentFormatName(sniffed) + L"，当前版本尚不支持打开该格式。";
    if (byExt) {
        message += L"扩展名像 " + archiveFormatName(*byExt) + L"，但内容与扩展名不符。";
    }
    message += L"目前可读取 ZIP、7z、RAR；可创建 ZIP、7z。";
    throw ArchiveError(ErrorKind::Other, message);
}

}  // namespace

bool isArchivePath(const std::wstring& path) {
    std::error_code error;
    if (path.empty() || fs::is_directory(path, error)) {
        return false;
    }
    if (formatFromExtension(path)) {
        return true;
    }
    if (unsupportedFromExtension(path) != ContentFormat::Unknown) {
        return true;
    }
    const std::wstring extension = lowerExtension(path);
    if (extension == L".zipx") {
        return true;
    }
    std::error_code existsError;
    if (!fs::is_regular_file(path, existsError)) {
        return false;
    }
    const ContentFormat sniffed = sniffContentFormat(path);
    return readableFromContent(sniffed).has_value() || isRecognizedUnsupported(sniffed);
}

ArchiveFormat formatFromPath(const std::wstring& path) {
    const auto byExt = formatFromExtension(path);
    const std::wstring extension = lowerExtension(path);
    ContentFormat sniffed = ContentFormat::Unknown;
    std::error_code existsError;
    if (fs::is_regular_file(path, existsError)) {
        sniffed = sniffContentFormat(path);
    }

    if (isRecognizedUnsupported(sniffed)) {
        throwUnsupported(sniffed, byExt);
    }
    if (extension == L".zipx") {
        throw ArchiveError(ErrorKind::Other,
                           L"识别到扩展名为 ZIPX，当前版本尚不支持打开 ZIPX。目前可读取 ZIP、7z、RAR；可创建 ZIP、7z。");
    }
    if (const ContentFormat pretended = unsupportedFromExtension(path); pretended != ContentFormat::Unknown &&
        !readableFromContent(sniffed)) {
        // Extension claims TAR/GZ/XZ/ISO and content did not override as a readable format.
        throwUnsupported(pretended, byExt);
    }

    if (const auto byContent = readableFromContent(sniffed)) {
        return *byContent;
    }
    if (byExt) {
        // Keep extension for SFX / unusual headers that did not match a simple magic.
        return *byExt;
    }
    throw ArchiveError(ErrorKind::Other, L"无法识别为可打开的压缩包。目前可读取 ZIP、7z、RAR；可创建 ZIP、7z。");
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
    // One archive alone still opens; multiple archives or archive+files need an explicit choice.
    if (archives.size() == 1 && others.empty()) {
        decision.action = DropAction::OpenArchive;
        decision.archivePath = archives.front();
        decision.archives = std::move(archives);
        return decision;
    }
    if (!archives.empty()) {
        decision.action = DropAction::Prompt;
        decision.archivePath = archives.front();
        decision.archives = std::move(archives);
        decision.files = std::move(others);
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
                rethrowBit(error, hooks.cancel != nullptr && hooks.cancel->load());
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

std::wstring keepBothName(const std::wstring& outputDir, const std::wstring& relativePath) {
    const fs::path root(outputDir);
    const fs::path relative(relativePath);
    const fs::path parent = relative.parent_path();
    const std::wstring stem = relative.stem().wstring();
    const std::wstring extension = relative.extension().wstring();
    for (int number = 2; number < 10000; ++number) {
        const fs::path candidate = parent / (stem + L" (" + std::to_wstring(number) + L")" + extension);
        std::error_code error;
        if (!fs::exists(root / candidate, error) && !error) {
            return candidate.wstring();
        }
    }
    throw ArchiveError(ErrorKind::Other, L"无法为同名文件找到可用的新名称：" + relativePath);
}

ExtractResult ArchiveService::extract(const ExtractRequest& request, const OperationHooks& hooks) const {
    return extract(request, hooks, nullptr);
}

ExtractResult ArchiveService::extract(const ExtractRequest& request, const OperationHooks& hooks,
                                      std::optional<ExtractResult>* interrupted) const {
    if (!fs::exists(request.archivePath)) {
        throw ArchiveError(ErrorKind::Other, L"找不到压缩包。");
    }
    if (!request.itemChoices.empty()) {
        // Split the batch by decision: Skip conflicts are left out, Overwrite
        // conflicts are extracted with Overwrite, everything else goes through
        // KeepBoth (which writes non-conflicting files and renames kept copies).
        const ArchiveInfo info = read(request.archivePath, request.password, hooks);
        const fs::path root(request.outputDir);
        ExtractRequest keepPass = request;
        keepPass.password = info.password;
        keepPass.itemChoices.clear();
        keepPass.indices.clear();
        keepPass.relativePaths.clear();
        keepPass.overwrite = OverwritePolicy::KeepBoth;
        ExtractRequest overwritePass = keepPass;
        overwritePass.overwrite = OverwritePolicy::Overwrite;
        overwritePass.removeOutputOnCancel = false;
        std::vector<std::wstring> skippedPaths;
        for (const ArchiveEntry& entry : info.entries) {
            if (entry.path.empty() || !included(request.indices, entry.index)) {
                continue;
            }
            if (entry.isDir) {
                keepPass.indices.push_back(entry.index);
                continue;
            }
            std::error_code error;
            const bool exists = fs::exists(root / entry.path, error);
            OverwritePolicy choice = request.overwrite;
            const auto found = request.itemChoices.find(entry.path);
            if (found != request.itemChoices.end()) {
                choice = found->second;
            }
            if (!exists || choice == OverwritePolicy::KeepBoth) {
                keepPass.indices.push_back(entry.index);
                keepPass.relativePaths.push_back(entry.path);
            } else if (choice == OverwritePolicy::Overwrite) {
                overwritePass.indices.push_back(entry.index);
                overwritePass.relativePaths.push_back(entry.path);
            } else {
                skippedPaths.push_back(entry.path);
            }
        }
        ExtractResult result;
        // If a pass stops early, report finished passes, the measured part of the
        // running pass, and every file of a pass that never started as not written.
        std::optional<ExtractResult> inner;
        const std::vector<std::wstring>* running = nullptr;
        std::vector<std::wstring> notStarted = overwritePass.relativePaths;
        try {
            // An empty index list means "everything", so only run passes with items.
            if (!keepPass.indices.empty()) {
                running = &keepPass.relativePaths;
                result = extract(keepPass, hooks, &inner);
                running = nullptr;
            }
            if (!overwritePass.indices.empty()) {
                if (hooks.cancel && hooks.cancel->load()) {
                    throw ArchiveError(ErrorKind::Cancelled, L"已取消");
                }
                inner.reset();
                running = &overwritePass.relativePaths;
                notStarted.clear();
                const ExtractResult replaced = extract(overwritePass, hooks, &inner);
                running = nullptr;
                result.overwritten += replaced.overwritten;
                result.written += replaced.written;
                result.missing.insert(result.missing.end(), replaced.missing.begin(), replaced.missing.end());
            }
        } catch (...) {
            if (interrupted) {
                ExtractResult partial = result;
                if (running && inner) {
                    appendResult(partial, *inner);
                } else if (running) {
                    partial.missing.insert(partial.missing.end(), running->begin(), running->end());
                }
                partial.missing.insert(partial.missing.end(), notStarted.begin(), notStarted.end());
                partial.skipped.insert(partial.skipped.end(), skippedPaths.begin(), skippedPaths.end());
                *interrupted = std::move(partial);
            }
            throw;
        }
        result.skipped.insert(result.skipped.end(), skippedPaths.begin(), skippedPaths.end());
        return result;
    }
    if (request.overwrite == OverwritePolicy::KeepBoth) {
        // Existing files are never opened for writing: non-conflicting items are
        // extracted with Skip, conflicting ones go to a private staging folder and
        // are then renamed beside the originals with a free numbered name.
        const ArchiveInfo info = read(request.archivePath, request.password, hooks);
        const fs::path root(request.outputDir);
        std::vector<std::uint32_t> conflictIndices;
        std::vector<std::wstring> conflictPaths;
        for (const ArchiveEntry& entry : info.entries) {
            if (entry.isDir || entry.path.empty() || !included(request.indices, entry.index)) {
                continue;
            }
            std::error_code error;
            if (fs::exists(root / entry.path, error)) {
                conflictIndices.push_back(entry.index);
                conflictPaths.push_back(entry.path);
            }
        }
        ExtractRequest first = request;
        first.password = info.password;
        first.overwrite = OverwritePolicy::Skip;
        ExtractResult result;
        std::optional<ExtractResult> firstPartial;
        bool firstDone = false;
        try {
            const ExtractResult firstPass = extract(first, hooks, &firstPartial);
            firstDone = true;
            result.written = firstPass.written;
            result.missing = firstPass.missing;
            if (conflictIndices.empty()) {
                return result;
            }
            if (hooks.cancel && hooks.cancel->load()) {
                throw ArchiveError(ErrorKind::Cancelled, L"已取消");
            }
            fs::path staging;
            const auto seed = static_cast<unsigned long long>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            for (int attempt = 0; attempt < 100 && staging.empty(); ++attempt) {
                const fs::path candidate = root / (L".qingxia-keep-" + std::to_wstring(seed + attempt));
                std::error_code error;
                if (fs::create_directory(candidate, error) && !error) {
                    staging = candidate;
                }
            }
            if (staging.empty()) {
                throw ArchiveError(ErrorKind::Other, L"无法创建保留两者所需的临时文件夹。");
            }
            struct StagingGuard {
                fs::path path;
                ~StagingGuard() {
                    std::error_code error;
                    fs::remove_all(path, error);  // Private folder created above.
                }
            } guard{staging};
            ExtractRequest second = first;
            second.outputDir = staging.wstring();
            second.indices = conflictIndices;
            second.relativePaths = conflictPaths;
            second.overwrite = OverwritePolicy::Overwrite;
            second.removeOutputOnCancel = false;
            extract(second, hooks);
            for (const std::wstring& path : conflictPaths) {
                if (hooks.cancel && hooks.cancel->load()) {
                    throw ArchiveError(ErrorKind::Cancelled, L"已取消");
                }
                const fs::path from = staging / path;
                if (!fs::is_regular_file(from)) {
                    throw ArchiveError(ErrorKind::Other, L"未能写出同名文件的副本：" + path);
                }
                const std::wstring renamed = keepBothName(request.outputDir, path);
                std::error_code error;
                fs::create_directories((root / renamed).parent_path(), error);
                error.clear();
                fs::rename(from, root / renamed, error);
                if (error) {
                    throw ArchiveError(ErrorKind::Other, L"无法保存同名文件的副本：" + renamed);
                }
                result.keptCopies.push_back(renamed);
            }
            return result;
        } catch (...) {
            if (interrupted && (firstDone || firstPartial)) {
                ExtractResult partial = result;
                if (!firstDone) {
                    // The first pass skipped conflicts on purpose; they are listed below.
                    partial.written = firstPartial->written;
                    partial.missing = firstPartial->missing;
                }
                // Copies are saved in conflict order, so the rest were never written.
                for (std::size_t i = partial.keptCopies.size(); i < conflictPaths.size(); ++i) {
                    partial.missing.push_back(conflictPaths[i]);
                }
                *interrupted = std::move(partial);
            }
            throw;
        }
    }
    ExtractRequest current = request;
    // The source archive must never become one of its own extracted outputs.
    // GUI callers provide selected paths; direct service/CLI callers are read here.
    std::vector<std::wstring> paths = current.relativePaths;
    if (paths.empty()) {
        const ArchiveInfo info = read(current.archivePath, current.password, hooks);
        current.password = info.password;
        for (const ArchiveEntry& entry : info.entries) {
            if (!entry.isDir && included(current.indices, entry.index)) {
                paths.push_back(entry.path);
            }
        }
    }
    for (const std::wstring& path : paths) {
        if (path.empty()) {
            continue;
        }
        std::error_code error;
        if (fs::equivalent(fs::path(current.outputDir) / path, current.archivePath, error)) {
            throw ArchiveError(ErrorKind::Other,
                               L"解压目标包含源压缩包自身，已停止以避免覆盖原压缩包。");
        }
    }
    // Remember which files already existed so the result reports what was
    // actually written, replaced or skipped instead of estimating it.
    const fs::path outputRoot(current.outputDir);
    std::vector<bool> existedBefore;
    std::vector<std::pair<std::uintmax_t, fs::file_time_type>> stampBefore;
    existedBefore.reserve(paths.size());
    stampBefore.reserve(paths.size());
    for (const std::wstring& path : paths) {
        std::error_code error;
        const fs::path target = outputRoot / path;
        const bool existed = !path.empty() && fs::is_regular_file(target, error);
        existedBefore.push_back(existed);
        std::uintmax_t size = 0;
        fs::file_time_type time{};
        if (existed) {
            size = fs::file_size(target, error);
            time = fs::last_write_time(target, error);
        }
        stampBefore.emplace_back(size, time);
    }
    const auto summarize = [&]() {
        ExtractResult result;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            if (paths[i].empty()) {
                continue;
            }
            if (existedBefore[i]) {
                if (current.overwrite == OverwritePolicy::Skip) {
                    result.skipped.push_back(paths[i]);
                } else {
                    ++result.overwritten;
                }
                continue;
            }
            std::error_code error;
            const fs::file_status status = fs::symlink_status(outputRoot / paths[i], error);
            if (fs::is_directory(status)) {
                continue;
            }
            if (fs::exists(status)) {
                ++result.written;
            } else {
                result.missing.push_back(paths[i]);
            }
        }
        return result;
    };
    // After a failure or cancel nothing is assumed: an existing file counts as
    // overwritten only if its size or modification time changed.
    const auto summarizeInterrupted = [&]() {
        ExtractResult result;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            if (paths[i].empty()) {
                continue;
            }
            const fs::path target = outputRoot / paths[i];
            std::error_code error;
            if (existedBefore[i]) {
                if (current.overwrite == OverwritePolicy::Skip) {
                    result.skipped.push_back(paths[i]);
                    continue;
                }
                const std::uintmax_t size = fs::file_size(target, error);
                const bool sizeRead = !error;
                error.clear();
                const fs::file_time_type time = fs::last_write_time(target, error);
                if (sizeRead && !error && (size != stampBefore[i].first || time != stampBefore[i].second)) {
                    ++result.overwritten;
                } else {
                    result.missing.push_back(paths[i]);
                }
                continue;
            }
            const fs::file_status status = fs::symlink_status(target, error);
            if (fs::is_directory(status)) {
                continue;
            }
            if (fs::exists(status)) {
                ++result.written;
            } else {
                result.missing.push_back(paths[i]);
            }
        }
        return result;
    };
    try {
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
                return summarize();
            } catch (const bit7z::BitException& error) {
                ArchiveError translated(ErrorKind::Other, L"");
                try {
                    rethrowBit(error, hooks.cancel != nullptr && hooks.cancel->load());
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
    } catch (...) {
        if (interrupted) {
            *interrupted = summarizeInterrupted();
        }
        throw;
    }
}

void ArchiveService::move(const ExtractRequest& request, const OperationHooks& hooks) const {
    if (request.indices.empty() || request.overwrite != OverwritePolicy::Overwrite) {
        throw ArchiveError(ErrorKind::Other, L"移动需要选择项目，并完整写入目标文件，不能跳过同名文件。");
    }
    const auto checkCancelled = [&]() {
        if (hooks.cancel && hooks.cancel->load())
            throw ArchiveError(ErrorKind::Cancelled, L"已取消移动，目标目录可能保留已复制的文件。");
    };
    checkCancelled();
    const auto info = read(request.archivePath, request.password, hooks);
    if (info.format == ArchiveFormat::Rar)
        throw ArchiveError(ErrorKind::Other, L"不能从 RAR 移除文件，请使用复制。");
    for (auto index : request.indices) {
        if (std::none_of(info.entries.begin(), info.entries.end(),
                         [index](const auto& entry) { return entry.index == index; }))
            throw ArchiveError(ErrorKind::Other, L"所选项目已变化，请重新打开压缩包后再试。");
    }
    ExtractRequest current = request;
    current.password = info.password;
    current.indices = expandSelection(info.entries, request.indices);
    current.relativePaths.clear();
    for (const auto& entry : info.entries) {
        if (!included(current.indices, entry.index)) continue;
        const fs::path relative(entry.path);
        if (entry.isLink || relative.empty() || relative.is_absolute() ||
            std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == L".."; }))
            throw ArchiveError(ErrorKind::Other, L"所选项目包含链接或异常路径，请先使用复制并检查文件。");
        current.relativePaths.push_back(entry.path);
    }
    extract(current, hooks);
    checkCancelled();

    // Read each written file back before deleting anything from the archive.
    static const auto crcTable = []() {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t i = 0; i < table.size(); ++i) {
            auto value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
            table[i] = value;
        }
        return table;
    }();
    std::array<char, 65536> buffer{};
    for (const auto& entry : info.entries) {
        if (!included(current.indices, entry.index)) continue;
        checkCancelled();
        if (hooks.onFile) hooks.onFile(L"正在核对：" + entry.path);
        const auto output = fs::path(current.outputDir) / entry.path;
        const auto mismatch = [&]() {
            return ArchiveError(ErrorKind::Other, L"目标文件核对未通过，尚未从压缩包移除任何项目：" + entry.path);
        };
        if (entry.isDir) {
            if (!fs::is_directory(output)) throw mismatch();
            continue;
        }
        if (!fs::is_regular_file(output) || fs::file_size(output) != entry.size) throw mismatch();
        std::ifstream input(output, std::ios::binary);
        if (!input) throw mismatch();
        std::uint32_t crc = 0xffffffffu;
        std::uint64_t bytes = 0;
        while (input) {
            checkCancelled();
            input.read(buffer.data(), buffer.size());
            const auto count = input.gcount();
            bytes += static_cast<std::uint64_t>(count);
            for (std::streamsize i = 0; i < count; ++i)
                crc = (crc >> 8) ^ crcTable[(crc ^ static_cast<unsigned char>(buffer[i])) & 0xff];
        }
        if (!input.eof() || bytes != entry.size || (entry.hasCrc && (crc ^ 0xffffffffu) != entry.crc))
            throw mismatch();
    }
    checkCancelled();
    try {
        removeItems(current.archivePath, current.indices, current.password, hooks);
    } catch (const ArchiveError& error) {
        throw ArchiveError(error.kind(), L"文件已复制并核对，但从压缩包移除未完成。请检查两处文件。\n" + error.message());
    }
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
    if (request.sfx && request.volumeSize > 0) {
        throw ArchiveError(ErrorKind::Other, L"自解压程序不能同时使用分卷压缩。");
    }
    const bool singleVolume = request.volumeSize == 0;
    const fs::path staged = stagingPath(output);
    const fs::path payload = request.sfx ? fs::path(staged.wstring() + L".payload.7z") : staged;
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
            writeSfx(findSfxModule(m_dllPath), payload, staged);
            removePath(payload);
        }
        if (singleVolume) {
            commitStagedFile(staged, output);
        } else {
            commitStagedVolumes(staged, output);
        }
    } catch (const bit7z::BitException& error) {
        if (singleVolume) removePath(staged);
        else removeStagedVolumes(staged);
        if (request.sfx) removePath(payload);
        ArchiveError translated(ErrorKind::Other, L"");
        try {
            rethrowBit(error, hooks.cancel != nullptr && hooks.cancel->load());
        } catch (const ArchiveError& caught) {
            translated = caught;
        }
        throw translated;
    } catch (...) {
        if (singleVolume) removePath(staged);
        else removeStagedVolumes(staged);
        if (request.sfx) removePath(payload);
        throw;
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
                rethrowBit(error, hooks.cancel != nullptr && hooks.cancel->load());
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
                                                   const OperationHooks& hooks,
                                                   std::uint64_t maxBytes) const {
    const ArchiveFormat format = formatFromPath(archivePath);
    const auto sizeLimitMessage = [maxBytes]() {
        return L"文件超过内存解压上限（" + std::to_wstring(maxBytes / (1024 * 1024)) +
               L" MB），请改为解压到磁盘后再打开。";
    };
    for (int attempt = 0; attempt < 5; ++attempt) {
        try {
            if (maxBytes > 0) {
                bit7z::BitArchiveReader reader{m_library->library, archivePath, toInFormat(format), password};
                const auto items = reader.items();
                bool found = false;
                for (const auto& item : items) {
                    if (item.index() != index) {
                        continue;
                    }
                    found = true;
                    if (!item.isDir() && item.size() > maxBytes) {
                        throw ArchiveError(ErrorKind::Other, sizeLimitMessage());
                    }
                    break;
                }
                if (!found) {
                    throw ArchiveError(ErrorKind::Other, L"找不到要读取的文件。");
                }
            }

            std::atomic_bool limitHit{false};
            std::atomic_bool combinedCancel{false};
            OperationHooks limited = hooks;
            limited.cancel = &combinedCancel;
            const auto userCancel = hooks.cancel;
            const auto userProgress = hooks.onProgress;
            const auto userTotal = hooks.onTotal;
            limited.onTotal = [userTotal, maxBytes, &limitHit, &combinedCancel](std::uint64_t total) {
                if (maxBytes > 0 && total > maxBytes) {
                    limitHit.store(true);
                    combinedCancel.store(true);
                }
                if (userTotal) {
                    userTotal(total);
                }
            };
            limited.onProgress = [userProgress, userCancel, maxBytes, &limitHit,
                                  &combinedCancel](std::uint64_t processed) {
                if (userCancel != nullptr && userCancel->load()) {
                    combinedCancel.store(true);
                }
                if (maxBytes > 0 && processed > maxBytes) {
                    limitHit.store(true);
                    combinedCancel.store(true);
                }
                if (userProgress) {
                    userProgress(processed);
                }
            };

            bit7z::BitFileExtractor extractor{m_library->library, toInFormat(format)};
            if (!password.empty()) {
                extractor.setPassword(password);
            }
            bindHooks(extractor, limited);
            std::vector<bit7z::byte_t> buffer;
            try {
                extractor.extract(archivePath, buffer, index);
            } catch (const bit7z::BitException&) {
                if (limitHit.load()) {
                    buffer.clear();
                    buffer.shrink_to_fit();
                    throw ArchiveError(ErrorKind::Other, sizeLimitMessage());
                }
                throw;
            }
            if (maxBytes > 0 && buffer.size() > maxBytes) {
                buffer.clear();
                buffer.shrink_to_fit();
                throw ArchiveError(ErrorKind::Other, sizeLimitMessage());
            }
            return std::vector<std::uint8_t>(buffer.begin(), buffer.end());
        } catch (const ArchiveError&) {
            throw;
        } catch (const bit7z::BitException& error) {
            ArchiveError translated(ErrorKind::Other, L"");
            try {
                rethrowBit(error, hooks.cancel != nullptr && hooks.cancel->load());
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
        if (hooks.cancel != nullptr && hooks.cancel->load()) {
            throw ArchiveError(ErrorKind::Cancelled, L"已取消");
        }
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
        } catch (const ArchiveError& error) {
            if (error.kind() == ErrorKind::Cancelled) {
                throw;
            }
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
                rethrowBit(error, hooks.cancel != nullptr && hooks.cancel->load());
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
                rethrowBit(error, hooks.cancel != nullptr && hooks.cancel->load());
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
