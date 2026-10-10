#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zipdesk {

enum class ArchiveFormat : int { Zip, SevenZip, Rar };

enum class CompressionLevel : int { Store, Fast, Normal, Maximum };

enum class OverwritePolicy : int { Overwrite, Skip };

enum class ErrorKind : int { Password, Cancelled, InvalidPasswordCharset, Other };

enum class DropAction : int { OpenArchive, AppendFiles, CreateArchive };

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
    bool removeOutputOnCancel = false;
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
    std::vector<std::wstring> files;
};

class ArchiveService {
public:
    explicit ArchiveService(std::wstring dllPath);
    ~ArchiveService();

    ArchiveService(const ArchiveService&) = delete;
    ArchiveService& operator=(const ArchiveService&) = delete;

    ArchiveInfo read(const std::wstring& archivePath, std::wstring password, const OperationHooks& hooks) const;
    void extract(const ExtractRequest& request, const OperationHooks& hooks) const;
    void compress(const CompressRequest& request, const OperationHooks& hooks) const;
    void append(const AppendRequest& request, const OperationHooks& hooks) const;
    void test(const std::wstring& archivePath, std::wstring password, const OperationHooks& hooks) const;
    std::vector<std::uint8_t> readItem(const std::wstring& archivePath,
                                       std::uint32_t index,
                                       std::wstring password,
                                       const OperationHooks& hooks) const;
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
ArchiveFormat formatFromPath(const std::wstring& path);

std::vector<std::uint32_t> expandSelection(const std::vector<ArchiveEntry>& entries,
                                           const std::vector<std::uint32_t>& selected);
bool hasConflicts(const std::vector<ArchiveEntry>& entries,
                  const std::vector<std::uint32_t>& indices,
                  const std::wstring& outputDir);
DropDecision decideDrop(const std::vector<std::wstring>& paths, bool archiveOpen);

}  // namespace zipdesk
