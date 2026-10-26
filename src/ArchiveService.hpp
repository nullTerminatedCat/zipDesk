#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zipdesk {

enum class ArchiveFormat : int { Zip, SevenZip, Rar, Tar, GZip, TarGz };

// Result of reading the file header (and a few common extensions). Readable formats
// include Zip/SevenZip/Rar/Tar/GZip/TarGz; Xz/Iso are recognized so we can explain the limit.
enum class ContentFormat : int { Unknown, Zip, SevenZip, Rar, Tar, GZip, Xz, Iso };

enum class CompressionLevel : int { Store, Fast, Normal, Maximum };

// KeepBoth leaves existing files untouched and writes conflicting archive files
// beside them with a numbered name such as "name (2).ext".
enum class OverwritePolicy : int { Overwrite, Skip, KeepBoth };

enum class ErrorKind : int { Password, Cancelled, InvalidPasswordCharset, Other };

enum class DropAction : int { OpenArchive, AppendFiles, CreateArchive, Prompt };

class ArchiveError : public std::exception {
public:
    ArchiveError(ErrorKind kind, std::wstring message);

    const char* what() const noexcept override;
    ErrorKind kind() const noexcept;
    const std::wstring& message() const noexcept;

private:
    ErrorKind m_kind;
    std::wstring m_message;
    std::string m_what;
};

struct ArchiveEntry {
    std::uint32_t index = 0;
    std::wstring path;
    std::wstring name;
    std::uint64_t size = 0;
    std::uint64_t packSize = 0;
    bool isDir = false;
    bool encrypted = false;
    bool hasModified = false;
    bool isLink = false;
    bool hasCrc = false;
    std::uint32_t crc = 0;
    std::wstring modified;
};

struct ArchiveInfo {
    std::wstring path;
    ArchiveFormat format = ArchiveFormat::Zip;
    std::wstring password;
    bool encrypted = false;
    std::wstring comment;
    std::vector<ArchiveEntry> entries;
};

struct OperationHooks {
    std::function<void(std::uint64_t total)> onTotal;
    std::function<void(std::uint64_t processed)> onProgress;
    std::function<void(const std::wstring& file)> onFile;
    std::function<std::optional<std::wstring>(const std::wstring& prompt)> askPassword;
    std::atomic<bool>* cancel = nullptr;
};

struct ExtractRequest {
    std::wstring archivePath;
    std::wstring outputDir;
    std::wstring password;
    std::vector<std::uint32_t> indices;
    std::vector<std::wstring> relativePaths;
    OverwritePolicy overwrite = OverwritePolicy::Overwrite;
    // Per-file decisions for existing targets, keyed by archive path. A conflicting
    // file without an entry falls back to `overwrite`. Non-conflicting files are
    // always written normally.
    std::map<std::wstring, OverwritePolicy> itemChoices;
    bool removeOutputOnCancel = false;
};

struct ExtractResult {
    // Relative paths (under outputDir) of files saved with a numbered name by KeepBoth.
    std::vector<std::wstring> keptCopies;
    // Measured on disk after a successful extraction; directories are not counted.
    std::size_t written = 0;            // files that did not exist before and exist now
    std::size_t overwritten = 0;        // existing files replaced under Overwrite
    std::vector<std::wstring> skipped;  // existing files left untouched under Skip
    std::vector<std::wstring> missing;  // requested files still absent afterwards
};

struct CompressRequest {
    std::vector<std::wstring> inputs;
    std::wstring outputPath;
    ArchiveFormat format = ArchiveFormat::Zip;
    CompressionLevel level = CompressionLevel::Normal;
    std::wstring password;
    std::uint64_t volumeSize = 0;
    bool solid = false;
    bool sfx = false;
};

struct AppendRequest {
    std::wstring archivePath;
    std::vector<std::wstring> inputs;
    std::wstring password;
    CompressionLevel level = CompressionLevel::Normal;
};

struct DropDecision {
    DropAction action = DropAction::CreateArchive;
    std::wstring archivePath;
    std::vector<std::wstring> archives;
    std::vector<std::wstring> files;
};

class ArchiveService {
public:
    explicit ArchiveService(std::wstring dllPath);
    ~ArchiveService();

    ArchiveService(const ArchiveService&) = delete;
    ArchiveService& operator=(const ArchiveService&) = delete;

    ArchiveInfo read(const std::wstring& archivePath, std::wstring password, const OperationHooks& hooks) const;
    ExtractResult extract(const ExtractRequest& request, const OperationHooks& hooks) const;
    // Same as above; when the extraction fails or is cancelled after it started
    // writing, `interrupted` receives the counts measured on disk at that point
    // (files not written are listed in `missing`) before the error is rethrown.
    ExtractResult extract(const ExtractRequest& request, const OperationHooks& hooks,
                          std::optional<ExtractResult>* interrupted) const;
    void move(const ExtractRequest& request, const OperationHooks& hooks) const;
    void compress(const CompressRequest& request, const OperationHooks& hooks) const;
    void append(const AppendRequest& request, const OperationHooks& hooks) const;
    void test(const std::wstring& archivePath, std::wstring password, const OperationHooks& hooks) const;
    // Soft cap for preview / in-window text edit. Callers that extract to disk
    // should not use this; pass 0 to readItem for an unlimited extract.
    static constexpr std::uint64_t kMaxInMemoryExtractBytes = 2ull * 1024 * 1024;

    // When maxBytes > 0, refuse if the entry's declared size exceeds the cap,
    // cancel mid-extract once processed bytes exceed it, and refuse an oversized
    // buffer even when the archive lied about size.
    std::vector<std::uint8_t> readItem(const std::wstring& archivePath,
                                       std::uint32_t index,
                                       std::wstring password,
                                       const OperationHooks& hooks,
                                       std::uint64_t maxBytes = 0) const;
    struct RepairResult {
        int recovered = 0;
        int failed = 0;
        std::wstring outputDir;
    };
    RepairResult repair(const std::wstring& archivePath, std::wstring password, const OperationHooks& hooks) const;
    void removeItems(const std::wstring& archivePath,
                     std::vector<std::uint32_t> indices,
                     std::wstring password,
                     const OperationHooks& hooks) const;
    void renameItem(const std::wstring& archivePath,
                    std::uint32_t index,
                    const std::wstring& newPath,
                    std::wstring password,
                    const OperationHooks& hooks) const;
    void replaceItem(const std::wstring& archivePath,
                     std::uint32_t index,
                     const std::wstring& filePath,
                     std::wstring password,
                     const OperationHooks& hooks) const;

private:
    std::wstring m_dllPath;
    class LibraryHolder;
    std::unique_ptr<LibraryHolder> m_library;
};

std::wstring normalizeArchivePath(std::wstring path);
bool pathInside(const std::wstring& parent, const std::wstring& child);
bool isArchivePath(const std::wstring& path);
ContentFormat sniffContentFormat(const std::wstring& path);
std::wstring contentFormatName(ContentFormat format);
ArchiveFormat formatFromPath(const std::wstring& path);
// RAR / TAR / GZIP / TAR.GZ are open-and-extract only.
bool isReadOnlyFormat(ArchiveFormat format);
std::wstring readableFormatsHint();

std::vector<std::uint32_t> expandSelection(const std::vector<ArchiveEntry>& entries,
                                           const std::vector<std::uint32_t>& selected);
bool hasConflicts(const std::vector<ArchiveEntry>& entries,
                  const std::vector<std::uint32_t>& indices,
                  const std::wstring& outputDir);
// Returns a relative path beside relativePath that does not exist under outputDir,
// e.g. "docs/a (2).txt". Only checks names; does not create anything.
std::wstring keepBothName(const std::wstring& outputDir, const std::wstring& relativePath);
struct SmartExtractDestination {
    std::wstring outputDir;
    std::wstring resultDir;
};
// Chooses paths only; does not create or overwrite anything.
SmartExtractDestination smartExtractDestination(const std::wstring& archivePath,
                                               const std::vector<ArchiveEntry>& entries);
DropDecision decideDrop(const std::vector<std::wstring>& paths, bool archiveOpen);

}  // namespace zipdesk
