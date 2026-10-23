#include "ArchiveService.hpp"
#include "Launch.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
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


void testContentSniff(const fs::path& root) {
    const auto writeBytes = [](const fs::path& path, const std::vector<unsigned char>& bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };

    const fs::path zipMisnamed = root / L"payload.dat";
    writeBytes(zipMisnamed, {'P', 'K', 0x03, 0x04, 0, 0, 0, 0});
    check(zipdesk::isArchivePath(zipMisnamed.wstring()), "zip magic without zip extension is archive");
    check(zipdesk::sniffContentFormat(zipMisnamed.wstring()) == zipdesk::ContentFormat::Zip, "sniff zip magic");
    check(zipdesk::formatFromPath(zipMisnamed.wstring()) == zipdesk::ArchiveFormat::Zip, "open misnamed zip by content");

    const fs::path sevenAsZip = root / L"actually.7z.zip";
    writeBytes(sevenAsZip, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C, 0, 0});
    check(zipdesk::sniffContentFormat(sevenAsZip.wstring()) == zipdesk::ContentFormat::SevenZip, "sniff 7z under zip name");
    check(zipdesk::formatFromPath(sevenAsZip.wstring()) == zipdesk::ArchiveFormat::SevenZip,
          "prefer 7z content over zip extension");

    const fs::path gzipAsZip = root / L"tarlike.zip";
    writeBytes(gzipAsZip, {0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0});
    bool rejectedGzip = false;
    std::wstring gzipMessage;
    try {
        (void)zipdesk::formatFromPath(gzipAsZip.wstring());
    } catch (const zipdesk::ArchiveError& error) {
        rejectedGzip = true;
        gzipMessage = error.message();
    }
    check(rejectedGzip, "gzip content under zip extension is rejected");
    check(gzipMessage.find(L"GZIP") != std::wstring::npos && gzipMessage.find(L"ZIP") != std::wstring::npos,
          "gzip mismatch mentions GZIP and ZIP extension");

    const fs::path tarFile = root / L"sample.tar";
    std::vector<unsigned char> tar(300, 0);
    const char ustar[] = "ustar";
    for (int i = 0; i < 5; ++i) tar[257 + i] = static_cast<unsigned char>(ustar[i]);
    writeBytes(tarFile, tar);
    check(zipdesk::isArchivePath(tarFile.wstring()), "tar path counts as archive for clearer errors");
    bool rejectedTar = false;
    std::wstring tarMessage;
    try {
        (void)zipdesk::formatFromPath(tarFile.wstring());
    } catch (const zipdesk::ArchiveError& error) {
        rejectedTar = true;
        tarMessage = error.message();
    }
    check(rejectedTar && tarMessage.find(L"TAR") != std::wstring::npos, "tar is recognized but unsupported");

    check(zipdesk::isArchivePath(L"C:\\missing.zip"), "missing zip path still detected by extension");
    check(zipdesk::formatFromPath(L"C:\\missing.zip") == zipdesk::ArchiveFormat::Zip,
          "missing zip path keeps extension format");
}

void testDropDecision() {
    const zipdesk::DropDecision open = zipdesk::decideDrop({L"C:\\a.zip"}, false);
    check(open.action == zipdesk::DropAction::OpenArchive && open.archivePath == L"C:\\a.zip"
              && open.archives.size() == 1,
          "single archive drop opens");

    const zipdesk::DropDecision mixed = zipdesk::decideDrop({L"C:\\a.zip", L"C:\\b.txt"}, false);
    check(mixed.action == zipdesk::DropAction::Prompt && mixed.archives.size() == 1
              && mixed.files.size() == 1 && mixed.archivePath == L"C:\\a.zip",
          "archive plus file prompts");

    const zipdesk::DropDecision multi = zipdesk::decideDrop({L"C:\\a.zip", L"C:\\b.7z", L"C:\\c.rar"}, false);
    check(multi.action == zipdesk::DropAction::Prompt && multi.archives.size() == 3 && multi.files.empty(),
          "multiple archives prompt");

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

void testPerItemChoices(zipdesk::ArchiveService& service, const fs::path& root) {
    const fs::path input = root / "input";
    for (const char* name : {"a.txt", "b.txt", "c.txt", "d.txt"}) writeFile(input / name, std::string("new-") + name);
    zipdesk::CompressRequest pack;
    for (const char* name : {"a.txt", "b.txt", "c.txt", "d.txt"}) pack.inputs.push_back((input / name).wstring());
    pack.outputPath = (root / "items.zip").wstring();
    service.compress(pack, {});
    const fs::path out = root / "out";
    for (const char* name : {"a.txt", "b.txt", "c.txt"}) writeFile(out / name, "OLD");

    zipdesk::ExtractRequest request;
    request.archivePath = pack.outputPath;
    request.outputDir = out.wstring();
    request.overwrite = zipdesk::OverwritePolicy::KeepBoth;
    request.itemChoices = {{L"a.txt", zipdesk::OverwritePolicy::Overwrite},
                           {L"b.txt", zipdesk::OverwritePolicy::Skip},
                           {L"c.txt", zipdesk::OverwritePolicy::KeepBoth}};
    const zipdesk::ExtractResult mixed = service.extract(request, {});
    check(readFile(out / "a.txt") == "new-a.txt", "per-item overwrite replaces only the chosen file");
    check(readFile(out / "b.txt") == "OLD", "per-item skip leaves the chosen file untouched");
    check(readFile(out / "c.txt") == "OLD" && readFile(out / "c (2).txt") == "new-c.txt",
          "per-item keep both saves a numbered copy");
    check(readFile(out / "d.txt") == "new-d.txt", "per-item choices still write non-conflicting files");
    check(mixed.written == 1 && mixed.overwritten == 1 && mixed.keptCopies.size() == 1 && mixed.skipped.size() == 1 &&
              mixed.skipped.front() == L"b.txt" && mixed.missing.empty(),
          "per-item result counts every decision");

    // Cancelled before anything is written: every decision is still accounted for.
    const fs::path stopped = root / "stopped";
    for (const char* name : {"a.txt", "b.txt", "c.txt"}) writeFile(stopped / name, "OLD");
    zipdesk::ExtractRequest stopRequest = request;
    stopRequest.outputDir = stopped.wstring();
    std::atomic<bool> stopCancel{false};
    zipdesk::OperationHooks stopHooks;
    stopHooks.cancel = &stopCancel;
    stopHooks.onTotal = [&stopCancel](std::uint64_t) { stopCancel.store(true); };
    std::optional<zipdesk::ExtractResult> stopCounts;
    bool stopCancelled = false;
    try {
        service.extract(stopRequest, stopHooks, &stopCounts);
    } catch (const zipdesk::ArchiveError& error) {
        stopCancelled = error.kind() == zipdesk::ErrorKind::Cancelled;
    }
    const auto listed = [](const std::vector<std::wstring>& paths, const wchar_t* name) {
        return std::find(paths.begin(), paths.end(), std::wstring(name)) != paths.end();
    };
    check(stopCancelled && stopCounts.has_value(), "cancelled per-item extraction reports measured counts");
    if (stopCounts) {
        const std::size_t accounted = stopCounts->written + stopCounts->overwritten + stopCounts->keptCopies.size() +
                                      stopCounts->skipped.size() + stopCounts->missing.size();
        check(accounted == 4 && stopCounts->overwritten == 0 && stopCounts->keptCopies.empty() &&
                  stopCounts->skipped.size() == 1 && stopCounts->skipped.front() == L"b.txt" &&
                  listed(stopCounts->missing, L"a.txt") && listed(stopCounts->missing, L"c.txt"),
              "cancelled per-item counts list skipped and unwritten files");
    }
    check(readFile(stopped / "a.txt") == "OLD" && readFile(stopped / "b.txt") == "OLD" &&
              readFile(stopped / "c.txt") == "OLD" && !fs::exists(stopped / "c (2).txt"),
          "cancelled per-item extraction leaves existing files untouched");

    // Only skipped conflicts selected: nothing may be extracted (an empty index list means "all").
    const fs::path onlySkip = root / "only-skip";
    writeFile(onlySkip / "a.txt", "OLD");
    zipdesk::ArchiveInfo info = service.read(pack.outputPath, L"", {});
    zipdesk::ExtractRequest skipOnly = request;
    skipOnly.outputDir = onlySkip.wstring();
    skipOnly.indices.clear();
    for (const auto& entry : info.entries)
        if (entry.path == L"a.txt") skipOnly.indices.push_back(entry.index);
    skipOnly.itemChoices = {{L"a.txt", zipdesk::OverwritePolicy::Skip}};
    const zipdesk::ExtractResult skipped = service.extract(skipOnly, {});
    check(readFile(onlySkip / "a.txt") == "OLD" && !fs::exists(onlySkip / "d.txt") && skipped.skipped.size() == 1 &&
              skipped.written == 0,
          "per-item skip of the whole selection extracts nothing else");

    // Files without an explicit choice fall back to the batch policy.
    const fs::path fallback = root / "fallback";
    for (const char* name : {"a.txt", "b.txt"}) writeFile(fallback / name, "OLD");
    zipdesk::ExtractRequest partial = request;
    partial.outputDir = fallback.wstring();
    partial.overwrite = zipdesk::OverwritePolicy::Skip;
    partial.itemChoices = {{L"a.txt", zipdesk::OverwritePolicy::Overwrite}};
    const zipdesk::ExtractResult fallbackResult = service.extract(partial, {});
    check(readFile(fallback / "a.txt") == "new-a.txt" && readFile(fallback / "b.txt") == "OLD" &&
              fallbackResult.skipped.size() == 1 && fallbackResult.written == 2,
          "per-item choices fall back to the batch policy");
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
    const zipdesk::ExtractResult skipResult = service.extract(skip, {});
    check(readFile(target) == "EXISTING", "skip keeps the existing file");
    check(skipResult.skipped.size() == 1 && skipResult.skipped.front() == helloPath && skipResult.written == 0 &&
              skipResult.overwritten == 0 && skipResult.missing.empty(),
          "skip reports the untouched file");

    zipdesk::ExtractRequest overwrite = skip;
    overwrite.overwrite = zipdesk::OverwritePolicy::Overwrite;
    const zipdesk::ExtractResult overwriteResult = service.extract(overwrite, {});
    check(readFile(target) == "hello", "overwrite replaces the existing file");
    check(overwriteResult.overwritten == 1 && overwriteResult.written == 0 && overwriteResult.skipped.empty() &&
              overwriteResult.missing.empty(),
          "overwrite reports the replaced file");

    std::size_t fileCount = 0;
    for (const zipdesk::ArchiveEntry& entry : info.entries) {
        if (!entry.isDir) ++fileCount;
    }
    zipdesk::ExtractRequest freshAll;
    freshAll.archivePath = archive.wstring();
    freshAll.outputDir = (root / "fresh-count").wstring();
    const zipdesk::ExtractResult freshResult = service.extract(freshAll, {});
    check(fileCount > 0 && freshResult.written == fileCount && freshResult.overwritten == 0 &&
              freshResult.skipped.empty() && freshResult.missing.empty(),
          "fresh extraction counts every written file");

    writeFile(target, "EXISTING");
    zipdesk::ExtractRequest keepBoth = skip;
    keepBoth.overwrite = zipdesk::OverwritePolicy::KeepBoth;
    const zipdesk::ExtractResult keptOnce = service.extract(keepBoth, {});
    const fs::path keptSecond = target.parent_path() / L"hello (2).txt";
    check(readFile(target) == "EXISTING", "keep both leaves the existing file untouched");
    check(keptOnce.keptCopies.size() == 1 && fs::path(keptOnce.keptCopies.front()).filename() == L"hello (2).txt",
          "keep both reports the numbered copy");
    check(keptOnce.written == 0 && keptOnce.skipped.empty() && keptOnce.overwritten == 0 && keptOnce.missing.empty(),
          "keep both does not report the saved copy as skipped");
    check(readFile(keptSecond) == "hello", "keep both writes the archive file beside the original");
    const zipdesk::ExtractResult keptTwice = service.extract(keepBoth, {});
    check(keptTwice.keptCopies.size() == 1 && readFile(target.parent_path() / L"hello (3).txt") == "hello" &&
              readFile(keptSecond) == "hello",
          "keep both picks the next free number without replacing earlier copies");
    bool stagingLeft = false;
    for (const auto& item : fs::directory_iterator(extracted)) {
        if (item.path().filename().wstring().rfind(L".qingxia-keep-", 0) == 0) stagingLeft = true;
    }
    check(!stagingLeft, "keep both removes its private staging folder");
    check(fs::path(zipdesk::keepBothName(extracted.wstring(), L"docs/report.tar.gz")).filename() ==
              L"report.tar (2).gz" &&
              zipdesk::keepBothName(extracted.wstring(), L".env") == L".env (2)",
          "keep both names keep the extension and handle dot files");
    fs::remove(keptSecond);
    fs::remove(target.parent_path() / L"hello (3).txt");

    writeFile(target, "PRESERVE");
    std::atomic<bool> extractCancel{false};
    zipdesk::OperationHooks extractHooks;
    extractHooks.cancel = &extractCancel;
    extractHooks.onTotal = [&extractCancel](std::uint64_t) { extractCancel.store(true); };
    overwrite.relativePaths = {helloPath};
    bool extractCancelled = false;
    std::optional<zipdesk::ExtractResult> cancelledCounts;
    try {
        service.extract(overwrite, extractHooks, &cancelledCounts);
    } catch (const zipdesk::ArchiveError& error) {
        extractCancelled = error.kind() == zipdesk::ErrorKind::Cancelled;
    }
    check(extractCancelled, "extraction can be cancelled");
    check(cancelledCounts && cancelledCounts->written == 0 && cancelledCounts->overwritten == 0 &&
              cancelledCounts->missing.size() == 1 && cancelledCounts->missing.front() == helloPath,
          "cancelled extraction reports the untouched existing file as not written");
    check(readFile(target) == "PRESERVE", "cancelled extraction keeps files in an existing destination");
    const fs::path shared = root / "cancelled-with-user-file";
    zipdesk::ExtractRequest cancelFresh = overwrite;
    cancelFresh.outputDir = shared.wstring();
    cancelFresh.removeOutputOnCancel = true;
    std::atomic<bool> freshCancel{false};
    zipdesk::OperationHooks freshHooks;
    freshHooks.cancel = &freshCancel;
    freshHooks.onTotal = [&](std::uint64_t) {
        writeFile(shared / "user.txt", "KEEP");
        freshCancel.store(true);
    };
    bool freshCancelled = false;
    try {
        service.extract(cancelFresh, freshHooks);
    } catch (const zipdesk::ArchiveError& error) {
        freshCancelled = error.kind() == zipdesk::ErrorKind::Cancelled;
    }
    check(freshCancelled, "new-directory extraction can be cancelled");
    check(readFile(shared / "user.txt") == "KEEP",
          "cancelling extraction preserves files added to the new destination");

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

    for (const auto format : {zipdesk::ArchiveFormat::Zip, zipdesk::ArchiveFormat::SevenZip}) {
        const std::wstring extension = format == zipdesk::ArchiveFormat::Zip ? L".zip" : L".7z";
        const fs::path destination = root / (L"existing" + extension);
        writeFile(destination, "ORIGINAL");
        cancel.store(false);
        zipdesk::CompressRequest replace = request;
        replace.outputPath = destination.wstring();
        replace.format = format;
        bool wasCancelled = false;
        try {
            service.compress(replace, hooks);
        } catch (const zipdesk::ArchiveError& error) {
            wasCancelled = error.kind() == zipdesk::ErrorKind::Cancelled;
        }
        check(wasCancelled, "replacing an archive can be cancelled");
        check(readFile(destination) == "ORIGINAL", "cancelled replacement preserves the original archive");
        for (const auto& item : fs::directory_iterator(root)) {
            check(item.path().filename().wstring().find(L"existing.zipdesk-") != 0,
                  "cancelled replacement leaves no staged file");
        }
        service.compress(replace, {});
        check(containsPath(service.read(destination.wstring(), L"", {}), L"big.bin", 8 * 1024 * 1024),
              "successful replacement commits a readable archive");
    }

    const fs::path sfx = root / "existing.exe";
    writeFile(sfx, "ORIGINAL SFX");
    cancel.store(false);
    zipdesk::CompressRequest sfxRequest = request;
    sfxRequest.outputPath = sfx.wstring();
    sfxRequest.format = zipdesk::ArchiveFormat::SevenZip;
    sfxRequest.sfx = true;
    bool sfxCancelled = false;
    try {
        service.compress(sfxRequest, hooks);
    } catch (const zipdesk::ArchiveError& error) {
        sfxCancelled = error.kind() == zipdesk::ErrorKind::Cancelled;
    }
    check(sfxCancelled, "self-extracting output can be cancelled");
    check(readFile(sfx) == "ORIGINAL SFX", "cancelled self-extracting output preserves the original");
    service.compress(sfxRequest, {});
    const std::string sfxBytes = readFile(sfx);
    check(sfxBytes.size() > 1024 && sfxBytes.substr(0, 2) == "MZ", "self-extracting output is committed");

    std::atomic<bool> repairCancel{true};
    zipdesk::OperationHooks repairHooks;
    repairHooks.cancel = &repairCancel;
    bool repairCancelled = false;
    try {
        service.repair(archive.wstring(), L"", repairHooks);
    } catch (const zipdesk::ArchiveError& error) {
        repairCancelled = error.kind() == zipdesk::ErrorKind::Cancelled;
    }
    check(repairCancelled, "recovering readable files propagates cancellation");

    const fs::path volumeSource = root / "volume.bin";
    {
        std::mt19937 generator(7);
        std::ofstream output(volumeSource, std::ios::binary);
        for (int i = 0; i < 64 * 1024; ++i) {
            output.put(static_cast<char>(generator() & 0xff));
        }
    }
    const fs::path volumeOutput = root / "volumes.zip";
    const fs::path firstVolume(volumeOutput.wstring() + L".001");
    const fs::path extraVolume(volumeOutput.wstring() + L".999");
    writeFile(volumeOutput, "OLD BASE");
    writeFile(firstVolume, "OLD FIRST");
    writeFile(extraVolume, "OLD EXTRA");
    zipdesk::CompressRequest volumeRequest;
    volumeRequest.inputs = {volumeSource.wstring()};
    volumeRequest.outputPath = volumeOutput.wstring();
    volumeRequest.format = zipdesk::ArchiveFormat::Zip;
    volumeRequest.level = zipdesk::CompressionLevel::Store;
    volumeRequest.volumeSize = 16 * 1024;
    cancel.store(false);
    bool volumesCancelled = false;
    try {
        service.compress(volumeRequest, hooks);
    } catch (const zipdesk::ArchiveError& error) {
        volumesCancelled = error.kind() == zipdesk::ErrorKind::Cancelled;
    }
    check(volumesCancelled, "multi-volume compression can be cancelled");
    check(readFile(volumeOutput) == "OLD BASE" && readFile(firstVolume) == "OLD FIRST" &&
              readFile(extraVolume) == "OLD EXTRA", "cancelled multi-volume output preserves old files");
    service.compress(volumeRequest, {});
    check(!fs::exists(volumeOutput) && !fs::exists(extraVolume), "new multi-volume output replaces obsolete files");
    check(readFile(firstVolume).substr(0, 2) == "PK" &&
              fs::exists(fs::path(volumeOutput.wstring() + L".002")), "multi-volume output is committed");
    for (const auto& item : fs::directory_iterator(root)) {
        const std::wstring name = item.path().filename().wstring();
        check(name.find(L"volumes.zipdesk-") != 0 && name.find(L".zipdesk-backup-") == std::wstring::npos,
              "multi-volume output leaves no staging or backup files");
    }
}

}  // namespace

void testSourceArchiveProtection(zipdesk::ArchiveService& service, const fs::path& root) {
    const fs::path input = root / "self-source" / "payload.zip";
    const fs::path safe = root / "self-source" / "safe.txt";
    writeFile(input, "PAYLOAD");
    writeFile(safe, "safe");

    zipdesk::CompressRequest compress;
    compress.inputs = {input.wstring(), safe.wstring()};
    compress.outputPath = (root / "self-staged.zip").wstring();
    compress.format = zipdesk::ArchiveFormat::Zip;
    service.compress(compress, {});

    const auto info = service.read(compress.outputPath, L"", {});
    std::wstring payloadPath;
    std::wstring safePath;
    std::uint32_t safeIndex = 0;
    for (const auto& entry : info.entries) {
        if (entry.name == L"payload.zip") payloadPath = entry.path;
        if (entry.name == L"safe.txt") {
            safePath = entry.path;
            safeIndex = entry.index;
        }
    }
    check(!payloadPath.empty() && !fs::path(payloadPath).is_absolute() && !safePath.empty(),
          "self-overwrite fixture has relative entries");
    if (payloadPath.empty() || fs::path(payloadPath).is_absolute() || safePath.empty()) return;

    const fs::path destination = root / "self-destination";
    const fs::path archive = destination / payloadPath;
    fs::create_directories(archive.parent_path());
    fs::rename(compress.outputPath, archive);

    zipdesk::ExtractRequest extract;
    extract.archivePath = archive.wstring();
    extract.outputDir = destination.wstring();
    extract.relativePaths = {payloadPath};
    for (int mode = 0; mode < 2; ++mode) {
        if (mode == 1) extract.relativePaths.clear();
        bool rejected = false;
        try {
            service.extract(extract, {});
        } catch (const zipdesk::ArchiveError& error) {
            rejected = error.kind() == zipdesk::ErrorKind::Other;
        }
        check(rejected, "extraction cannot overwrite its source archive");
        check(containsPath(service.read(archive.wstring(), L"", {}), L"payload.zip", 7),
              "rejected self-overwrite preserves a readable source archive");
    }

    extract.indices = {safeIndex};
    extract.relativePaths = {safePath};
    service.extract(extract, {});
    check(readFile(destination / safePath) == "safe",
          "unrelated selected extraction remains available");
}
void testSmartExtractDestination() {
    const fs::path archive = fs::absolute(L"smart-fixture.zip");
    const auto parent = archive.parent_path();
    const auto entry = [](const std::wstring& path, bool directory = false) {
        zipdesk::ArchiveEntry item;
        item.path = path;
        item.isDir = directory;
        return item;
    };
    const auto choose = [&](std::vector<zipdesk::ArchiveEntry> entries) {
        return zipdesk::smartExtractDestination(archive.wstring(), entries);
    };
    auto result = choose({entry(L"docs/", true), entry(L"docs/a.txt"), entry(L"docs/sub/b.txt")});
    check(fs::path(result.outputDir) == parent && fs::path(result.resultDir) == parent / L"docs",
          "smart extraction avoids duplicate wrapper for one explicit directory");
    result = choose({entry(L"docs\\a.txt"), entry(L"docs\\sub\\b.txt")});
    check(fs::path(result.outputDir) == parent, "implicit directory and Windows separators are recognized");
    result = choose({entry(L"empty", true)});
    check(fs::path(result.resultDir) == parent / L"empty", "one empty directory keeps its name");
    for (const auto& entries : std::vector<std::vector<zipdesk::ArchiveEntry>>{
             {entry(L"readme.txt")}, {entry(L"docs/a"), entry(L"other/b")},
             {entry(L"docs/a"), entry(L"readme.txt")},
             {entry(L"docs"), entry(L"docs/a")}}) {
        result = choose(entries);
        check(fs::path(result.outputDir) == parent / L"smart-fixture",
              "multiple roots or root file get a named wrapper");
    }
    for (const auto& entries : std::vector<std::vector<zipdesk::ArchiveEntry>>{
             {}, {entry(L"../escape")}, {entry(L"/absolute")}, {entry(L"C:\\absolute")},
             {entry(L"docs/../escape")}, {entry(L"docs./file")}, {entry(L"docs//file")}}) {
        bool rejected = false;
        try { choose(entries); } catch (const zipdesk::ArchiveError&) { rejected = true; }
        check(rejected, "empty or ambiguous smart extraction paths are rejected");
    }
    const auto command = zipdesk::parseLaunch({L"ZipDesk.exe", L"--extract-smart", archive.wstring()});
    check(command.action == zipdesk::LaunchAction::SmartExtract && command.paths.size() == 1,
          "smart extract shell command is parsed");
    check(zipdesk::parseLaunch({L"ZipDesk.exe", L"--extract-smart"}).action == zipdesk::LaunchAction::None,
          "smart extract without a path does not launch");
}
void testMove(zipdesk::ArchiveService& service, const fs::path& root, zipdesk::ArchiveFormat format) {
    for (const std::string mode : {"overwrite", "cancel", "failure", "tamper", "cancel-verify", "skip"}) {
        const auto work = root / mode;
        writeFile(work / "source/sample.txt", "sample");
        writeFile(work / "source/keep.txt", "keep");
        zipdesk::CompressRequest compression;
        compression.inputs = {(work / "source/sample.txt").wstring(), (work / "source/keep.txt").wstring()};
        compression.outputPath = (work / (format == zipdesk::ArchiveFormat::Zip ? "archive.zip" : "archive.7z")).wstring();
        compression.format = format;
        service.compress(compression, {});
        const auto before = readFile(compression.outputPath);
        const auto info = service.read(compression.outputPath, L"", {});
        zipdesk::ExtractRequest move;
        move.archivePath = compression.outputPath;
        move.outputDir = (work / "output").wstring();
        for (const auto& entry : info.entries)
            if (entry.path == L"sample.txt") move.indices = {entry.index};
        check(move.indices.size() == 1, "move fixture has a selected item");
        const auto target = work / "output/sample.txt";
        if (mode == "failure") writeFile(target / "blocker.txt", "keep");
        else writeFile(target, "existing");
        std::atomic<bool> cancel{mode == "cancel"};
        zipdesk::OperationHooks hooks;
        hooks.cancel = &cancel;
        hooks.onFile = [&](const std::wstring& path) {
            if (path.find(L"正在核对：") != 0) return;
            if (mode == "tamper") writeFile(target, "tamper");
            if (mode == "cancel-verify") cancel.store(true);
        };
        if (mode == "skip") move.overwrite = zipdesk::OverwritePolicy::Skip;
        bool failed = false;
        try { service.move(move, hooks); }
        catch (const std::exception&) { failed = true; }
        check(failed == (mode != "overwrite"), "move outcome matches scenario");
        if (mode == "overwrite") {
            check(readFile(target) == "sample", "move replaces the destination with the selected file");
            const auto after = service.read(move.archivePath, L"", {});
            check(after.entries.size() == 1 && containsPath(after, L"keep.txt", 4),
                  "move removes only the verified selected item");
        } else {
            check(readFile(move.archivePath) == before, "failed or cancelled move preserves original archive bytes");
            service.test(move.archivePath, L"", {});
        }
    }
    const auto work = root / "directory";
    writeFile(work / "source/folder/nested/child.txt", "child");
    writeFile(work / "source/keep.txt", "keep");
    zipdesk::CompressRequest compression;
    compression.inputs = {(work / "source/folder").wstring(), (work / "source/keep.txt").wstring()};
    compression.outputPath = (work / (format == zipdesk::ArchiveFormat::Zip ? "archive.zip" : "archive.7z")).wstring();
    compression.format = format;
    service.compress(compression, {});
    const auto info = service.read(compression.outputPath, L"", {});
    zipdesk::ExtractRequest move;
    move.archivePath = compression.outputPath;
    move.outputDir = (work / "output").wstring();
    for (const auto& entry : info.entries)
        if (entry.isDir && (entry.path == L"folder" || entry.path == L"folder/")) move.indices = {entry.index};
    service.move(move, {});
    check(readFile(work / "output/folder/nested/child.txt") == "child", "moving a folder includes descendants");
    const auto after = service.read(move.archivePath, L"", {});
    check(after.entries.size() == 1 && containsPath(after, L"keep.txt", 4), "folder move preserves unrelated items");
}

int main() {
    testSmartExtractDestination();
    testLaunchParser();
    testDropDecision();

    const fs::path root = fs::temp_directory_path() / L"zipdesk-smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    testContentSniff(root / L"sniff");

    try {
        zipdesk::ArchiveService service(exeDirectory() + L"\\7z.dll");
        testRoundTrip(service, zipdesk::ArchiveFormat::Zip, root / "zip");
        testRoundTrip(service, zipdesk::ArchiveFormat::SevenZip, root / "7z");
        testOverwriteAndCancel(service, root / "zip");
        testSourceArchiveProtection(service, root / "zip");
        testPerItemChoices(service, root / "per-item");
        testMove(service, root / "move-zip", zipdesk::ArchiveFormat::Zip);
        testMove(service, root / "move-7z", zipdesk::ArchiveFormat::SevenZip);
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
