#include "ArchiveService.hpp"
#include "Launch.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

std::wstring exeDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(buffer).parent_path().wstring();
}

void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::string readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool containsPath(const zipdesk::ArchiveInfo& info, const std::wstring& suffix, std::uint64_t size) {
    for (const zipdesk::ArchiveEntry& entry : info.entries) {
        if (!entry.isDir && entry.path.size() >= suffix.size() &&
            entry.path.compare(entry.path.size() - suffix.size(), suffix.size(), suffix) == 0 && entry.size == size) {
            return true;
        }
    }
    return false;
}

fs::path findChild(const fs::path& root, const std::wstring& name) {
    for (const auto& item : fs::recursive_directory_iterator(root)) {
        if (item.path().filename() == name) {
            return item.path();
        }
    }
    return {};
}

void testLaunchParser() {
    const zipdesk::LaunchCommand empty = zipdesk::parseLaunch({L"ZipDesk.exe"});
    check(empty.action == zipdesk::LaunchAction::None, "no args opens the window");

    const zipdesk::LaunchCommand open = zipdesk::parseLaunch({L"ZipDesk.exe", L"C:\\a.zip"});
    check(open.action == zipdesk::LaunchAction::Open && open.paths.size() == 1, "path opens an archive");

    const zipdesk::LaunchCommand here = zipdesk::parseLaunch({L"ZipDesk.exe", L"--extract-here", L"C:\\a.7z"});
    check(here.action == zipdesk::LaunchAction::ExtractHere && here.paths.front() == L"C:\\a.7z", "extract here");

    const zipdesk::LaunchCommand folder = zipdesk::parseLaunch({L"ZipDesk.exe", L"--extract-to-folder", L"C:\\a.zip"});
    check(folder.action == zipdesk::LaunchAction::ExtractToFolder, "extract to folder");

    const zipdesk::LaunchCommand zip = zipdesk::parseLaunch({L"ZipDesk.exe", L"--compress-zip", L"C:\\docs"});
    check(zip.action == zipdesk::LaunchAction::CompressZip, "compress zip");

    const zipdesk::LaunchCommand seven = zipdesk::parseLaunch({L"ZipDesk.exe", L"--compress-7z", L"C:\\docs"});
    check(seven.action == zipdesk::LaunchAction::Compress7z, "compress 7z");

    const zipdesk::LaunchCommand missing = zipdesk::parseLaunch({L"ZipDesk.exe", L"--extract-here"});
    check(missing.action == zipdesk::LaunchAction::None, "missing path is ignored");
}

void testDropDecision() {
    const zipdesk::DropDecision open = zipdesk::decideDrop({L"C:\\a.zip", L"C:\\b.txt"}, false);
    check(open.action == zipdesk::DropAction::OpenArchive && open.archivePath == L"C:\\a.zip", "archive drop opens");

    const zipdesk::DropDecision create = zipdesk::decideDrop({L"C:\\b.txt", L"C:\\dir"}, false);
    check(create.action == zipdesk::DropAction::CreateArchive && create.files.size() == 2, "files drop creates");

    const zipdesk::DropDecision append = zipdesk::decideDrop({L"C:\\b.txt"}, true);
    check(append.action == zipdesk::DropAction::AppendFiles && append.files.size() == 1, "files drop appends");
}

void testRoundTrip(zipdesk::ArchiveService& service, zipdesk::ArchiveFormat format, const fs::path& root) {
    const fs::path source = root / "src";
    writeFile(source / "hello.txt", "hello");
    writeFile(source / "sub" / "world.txt", "world");
    const std::wstring extension = format == zipdesk::ArchiveFormat::Zip ? L".zip" : L".7z";
    const fs::path archive = root / (L"pack" + extension);

    zipdesk::CompressRequest compress;
    compress.inputs.push_back(source.wstring());
    compress.outputPath = archive.wstring();
    compress.format = format;
    compress.level = zipdesk::CompressionLevel::Normal;
    service.compress(compress, {});

    const zipdesk::ArchiveInfo info = service.read(archive.wstring(), L"", {});
    check(info.format == format, "format matches");
    check(containsPath(info, L"hello.txt", 5), "hello.txt is listed");
    check(containsPath(info, L"world.txt", 5), "world.txt is listed");
    check(!info.entries.empty() && info.entries.front().hasModified, "modified time is listed");

    const fs::path extracted = root / "out";
    zipdesk::ExtractRequest extract;
    extract.archivePath = archive.wstring();
    extract.outputDir = extracted.wstring();
    service.extract(extract, {});
    check(readFile(findChild(extracted, L"hello.txt")) == "hello", "hello.txt round trip");
    check(readFile(findChild(extracted, L"world.txt")) == "world", "world.txt round trip");

    std::uint32_t helloIndex = 0;
    for (const zipdesk::ArchiveEntry& entry : info.entries) {
        if (entry.path.size() >= 9 && entry.path.compare(entry.path.size() - 9, 9, L"hello.txt") == 0) {
            helloIndex = entry.index;
        }
    }
    const fs::path partial = root / "partial";
    zipdesk::ExtractRequest selected;
    selected.archivePath = archive.wstring();
    selected.outputDir = partial.wstring();
    selected.indices = {helloIndex};
    service.extract(selected, {});
    check(readFile(findChild(partial, L"hello.txt")) == "hello", "selected extract");
    check(findChild(partial, L"world.txt").empty(), "unselected file stays out");

    const fs::path extra = root / "extra.txt";
    writeFile(extra, "extra");
    zipdesk::AppendRequest append;
    append.archivePath = archive.wstring();
    append.inputs.push_back(extra.wstring());
    service.append(append, {});
    const zipdesk::ArchiveInfo appended = service.read(archive.wstring(), L"", {});
    check(containsPath(appended, L"extra.txt", 5), "append adds a file");

    const fs::path locked = root / (L"locked" + extension);
    zipdesk::CompressRequest secret = compress;
    secret.outputPath = locked.wstring();
    secret.password = L"secret";
    service.compress(secret, {});

    bool rejected = false;
    try {
        zipdesk::ExtractRequest wrong;
        wrong.archivePath = locked.wstring();
        wrong.outputDir = (root / "bad").wstring();
        wrong.password = L"nope";
        service.extract(wrong, {});
    } catch (const zipdesk::ArchiveError& error) {
        rejected = error.kind() == zipdesk::ErrorKind::Password;
    }
    check(rejected, "wrong password is rejected");

    int asks = 0;
    zipdesk::OperationHooks hooks;
    hooks.askPassword = [&asks](const std::wstring&) -> std::optional<std::wstring> {
        ++asks;
        return asks == 1 ? std::optional<std::wstring>(L"nope") : std::optional<std::wstring>(L"secret");
    };
    zipdesk::ExtractRequest retry;
    retry.archivePath = locked.wstring();
    retry.outputDir = (root / "good").wstring();
    service.extract(retry, hooks);
    check(asks >= 2, "password can be retried");
    check(readFile(findChild(root / "good", L"hello.txt")) == "hello", "correct password extracts");
}

void testOverwriteAndCancel(zipdesk::ArchiveService& service, const fs::path& root) {
    const fs::path archive = root / "pack.zip";
    const fs::path extracted = root / "out";
    const fs::path existing = findChild(extracted, L"hello.txt");
    writeFile(existing, "EXISTING");

    zipdesk::ArchiveInfo info = service.read(archive.wstring(), L"", {});
    std::uint32_t helloIndex = 0;
    std::wstring helloPath;
    for (const zipdesk::ArchiveEntry& entry : info.entries) {
        if (entry.path.size() >= 9 && entry.path.compare(entry.path.size() - 9, 9, L"hello.txt") == 0) {
            helloIndex = entry.index;
            helloPath = entry.path;
        }
    }
    const fs::path target = extracted / helloPath;
    writeFile(target, "EXISTING");

    zipdesk::ExtractRequest skip;
    skip.archivePath = archive.wstring();
    skip.outputDir = extracted.wstring();
    skip.indices = {helloIndex};
    skip.overwrite = zipdesk::OverwritePolicy::Skip;
    service.extract(skip, {});
    check(readFile(target) == "EXISTING", "skip keeps the existing file");

    zipdesk::ExtractRequest overwrite = skip;
    overwrite.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(overwrite, {});
    check(readFile(target) == "hello", "overwrite replaces the existing file");

    const fs::path big = root / "big.bin";
    {
        std::ofstream output(big, std::ios::binary);
        std::vector<char> block(1024 * 1024, '\0');
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = static_cast<char>(i * 17);
        }
        for (int i = 0; i < 8; ++i) {
            output.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    const fs::path cancelled = root / "cancelled.zip";
    std::atomic<bool> cancel{false};
    zipdesk::OperationHooks hooks;
    hooks.cancel = &cancel;
    hooks.onTotal = [&cancel](std::uint64_t) { cancel.store(true); };
    zipdesk::CompressRequest request;
    request.inputs.push_back(big.wstring());
    request.outputPath = cancelled.wstring();
    request.format = zipdesk::ArchiveFormat::Zip;
    request.level = zipdesk::CompressionLevel::Maximum;
    bool cancelledOk = false;
    try {
        service.compress(request, hooks);
    } catch (const zipdesk::ArchiveError& error) {
        cancelledOk = error.kind() == zipdesk::ErrorKind::Cancelled;
    }
    check(cancelledOk, "compression can be cancelled");
    check(!fs::exists(cancelled), "cancelled output is removed");
}

}  // namespace

int main() {
    testLaunchParser();
    testDropDecision();

    const fs::path root = fs::temp_directory_path() / L"zipdesk-smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    try {
        zipdesk::ArchiveService service(exeDirectory() + L"\\7z.dll");
        testRoundTrip(service, zipdesk::ArchiveFormat::Zip, root / "zip");
        testRoundTrip(service, zipdesk::ArchiveFormat::SevenZip, root / "7z");
        testOverwriteAndCancel(service, root / "zip");
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: exception " << ex.what() << "\n";
        ++g_failures;
    }

    fs::remove_all(root, error);
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "SMOKE OK\n";
    return 0;
}
