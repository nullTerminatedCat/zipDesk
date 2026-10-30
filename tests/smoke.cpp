#include "ArchiveService.hpp"
#include "Launch.hpp"
#include "ClassicContextMenu.hpp"

#include <bit7z/bit7z.hpp>
#include <bit7z/bitformat.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
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
    check(zipdesk::sniffContentFormat(gzipAsZip.wstring()) == zipdesk::ContentFormat::GZip, "sniff gzip under zip name");
    check(zipdesk::formatFromPath(gzipAsZip.wstring()) == zipdesk::ArchiveFormat::GZip,
          "prefer gzip content over zip extension");

    const fs::path tarFile = root / L"sample.tar";
    std::vector<unsigned char> tar(300, 0);
    const char ustar[] = "ustar";
    for (int i = 0; i < 5; ++i) tar[257 + i] = static_cast<unsigned char>(ustar[i]);
    writeBytes(tarFile, tar);
    check(zipdesk::isArchivePath(tarFile.wstring()), "tar path counts as archive");
    check(zipdesk::formatFromPath(tarFile.wstring()) == zipdesk::ArchiveFormat::Tar, "tar content is readable");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::Tar), "tar is read-only");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::GZip), "gzip is read-only");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::TarGz), "tar.gz is read-only");

    check(zipdesk::isArchivePath(L"C:\\missing.zip"), "missing zip path still detected by extension");
    check(zipdesk::formatFromPath(L"C:\\missing.zip") == zipdesk::ArchiveFormat::Zip,
          "missing zip path keeps extension format");
    check(zipdesk::formatFromPath(L"C:\\missing.tar.gz") == zipdesk::ArchiveFormat::TarGz,
          "missing tar.gz path keeps compound extension");
    check(zipdesk::formatFromPath(L"C:\\missing.tgz") == zipdesk::ArchiveFormat::TarGz,
          "missing tgz path keeps extension");
    check(zipdesk::formatFromPath(L"C:\\missing.tar") == zipdesk::ArchiveFormat::Tar,
          "missing tar path keeps extension");
    check(zipdesk::formatFromPath(L"C:\\missing.gz") == zipdesk::ArchiveFormat::GZip,
          "missing gz path keeps extension");

    const fs::path xzFile = root / L"sample.xz";
    writeBytes(xzFile, {0xFD, '7', 'z', 'X', 'Z', 0x00});
    check(zipdesk::sniffContentFormat(xzFile.wstring()) == zipdesk::ContentFormat::Xz, "sniff xz magic");
    check(zipdesk::formatFromPath(xzFile.wstring()) == zipdesk::ArchiveFormat::Xz, "xz content is readable");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::Xz), "xz is read-only");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::TarXz), "tar.xz is read-only");
    check(zipdesk::formatFromPath(L"C:\\missing.xz") == zipdesk::ArchiveFormat::Xz,
          "missing xz path keeps extension");
    check(zipdesk::formatFromPath(L"C:\\missing.tar.xz") == zipdesk::ArchiveFormat::TarXz,
          "missing tar.xz path keeps compound extension");
    check(zipdesk::formatFromPath(L"C:\\missing.txz") == zipdesk::ArchiveFormat::TarXz,
          "missing txz path keeps TarXz");

    // Minimal CD001 signature so sniff recognizes ISO without a full image.
    std::vector<unsigned char> isoHeader(0x8008, 0);
    const char cd001[] = "CD001";
    for (int i = 0; i < 5; ++i) isoHeader[0x8001 + i] = static_cast<unsigned char>(cd001[i]);
    writeBytes(root / L"named.iso", isoHeader);
    check(zipdesk::sniffContentFormat((root / L"named.iso").wstring()) == zipdesk::ContentFormat::Iso,
          "sniff iso CD001");
    check(zipdesk::formatFromPath((root / L"named.iso").wstring()) == zipdesk::ArchiveFormat::Iso,
          "iso content is readable");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::Iso), "iso is read-only");
    check(zipdesk::formatFromPath(L"C:\\missing.iso") == zipdesk::ArchiveFormat::Iso,
          "missing iso path keeps extension");

    writeBytes(root / L"named.zipx", {'P', 'K', 0x03, 0x04});
    check(zipdesk::formatFromPath((root / L"named.zipx").wstring()) == zipdesk::ArchiveFormat::Zipx,
          "zipx with ZIP header is Zipx");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::Zipx), "zipx is read-only");
    check(zipdesk::formatFromPath(L"C:\\missing.zipx") == zipdesk::ArchiveFormat::Zipx,
          "missing zipx path keeps extension");
    check(zipdesk::isArchivePath((root / L"named.zipx").wstring()), "zipx is archive path");

    check(zipdesk::formatFromPath(L"C:\\missing.zip.001") == zipdesk::ArchiveFormat::SplitZip,
          "missing zip.001 keeps split-zip format");
    check(zipdesk::formatFromPath(L"C:\\missing.7z.001") == zipdesk::ArchiveFormat::SplitSevenZip,
          "missing 7z.001 keeps split-7z format");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::SplitZip), "split zip is read-only");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::SplitSevenZip), "split 7z is read-only");
    check(zipdesk::isNumberedVolumePath(L"C:\\a.zip.001"), "zip.001 is numbered volume");
    check(zipdesk::isNumberedVolumePath(L"C:\\a.zip.002"), "zip.002 is numbered volume");
    check(!zipdesk::isNumberedVolumePath(L"C:\\a.zip"), "plain zip is not numbered volume");
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


void testReadItemSizeLimit(zipdesk::ArchiveService& service, const fs::path& root) {
    const fs::path source = root / "src";
    const fs::path archive = root / "limit.zip";
    writeFile(source / "small.txt", "ok");
    writeFile(source / "big.bin", std::string(3 * 1024 * 1024, 'A'));

    zipdesk::CompressRequest request;
    request.inputs = {source.wstring()};
    request.outputPath = archive.wstring();
    request.format = zipdesk::ArchiveFormat::Zip;
    request.level = zipdesk::CompressionLevel::Store;
    service.compress(request, {});

    const zipdesk::ArchiveInfo info = service.read(archive.wstring(), L"", {});
    std::uint32_t smallIndex = 0;
    std::uint32_t bigIndex = 0;
    bool haveSmall = false;
    bool haveBig = false;
    for (const auto& entry : info.entries) {
        if (entry.isDir) continue;
        if (entry.name == L"small.txt") {
            smallIndex = entry.index;
            haveSmall = true;
        }
        if (entry.name == L"big.bin") {
            bigIndex = entry.index;
            haveBig = true;
            check(entry.size == 3ull * 1024 * 1024, "big entry reports declared size");
        }
    }
    check(haveSmall && haveBig, "limit archive has small and big entries");

    const zipdesk::OperationHooks noHooks;
    const std::vector<std::uint8_t> smallBytes = service.readItem(
        archive.wstring(), smallIndex, L"", noHooks, zipdesk::ArchiveService::kMaxInMemoryExtractBytes);
    check(smallBytes.size() == 2 && smallBytes[0] == 'o' && smallBytes[1] == 'k',
          "small item reads under limit");

    bool rejected = false;
    std::wstring message;
    try {
        (void)service.readItem(archive.wstring(), bigIndex, L"", noHooks,
                               zipdesk::ArchiveService::kMaxInMemoryExtractBytes);
    } catch (const zipdesk::ArchiveError& error) {
        rejected = true;
        message = error.message();
    }
    check(rejected, "oversized item refused by readItem maxBytes");
    check(message.find(L"内存解压上限") != std::wstring::npos && message.find(L"2") != std::wstring::npos,
          "size-limit error mentions in-memory cap");

    const std::vector<std::uint8_t> unlimited = service.readItem(archive.wstring(), bigIndex, L"", noHooks, 0);
    check(unlimited.size() == 3ull * 1024 * 1024, "unlimited readItem still returns full payload");
}


void testClassicContextMenuRegistry() {
    const bool win11 = zipdesk::isWindows11ForClassicMenu();
    check(win11, "classic menu helper should detect Windows 11 on this CI machine");
    if (!win11) {
        return;
    }
    const bool original = zipdesk::isClassicContextMenuEnabled();
    check(zipdesk::enableClassicContextMenu(), "enable classic context menu writes InprocServer32");
    check(zipdesk::isClassicContextMenuEnabled(), "classic context menu reports enabled after write");
    check(zipdesk::disableClassicContextMenu(), "disable classic context menu deletes CLSID key");
    check(!zipdesk::isClassicContextMenuEnabled(), "classic context menu reports disabled after delete");
    check(zipdesk::enableClassicContextMenu(), "re-enable classic context menu succeeds");
    check(zipdesk::isClassicContextMenuEnabled(), "classic context menu enabled again");
    if (original) {
        check(zipdesk::enableClassicContextMenu(), "restore original enabled classic menu");
    } else {
        check(zipdesk::disableClassicContextMenu(), "restore original disabled classic menu");
    }
    check(zipdesk::isClassicContextMenuEnabled() == original, "classic menu state restored");
}


void testTarGzipRead(zipdesk::ArchiveService& service, const fs::path& root) {
    fs::create_directories(root / L"src");
    writeFile(root / L"src" / L"hello.txt", "hello-tar-gz");
    bit7z::Bit7zLibrary library(exeDirectory() + L"\\7z.dll");

    const fs::path tarPath = root / L"sample.tar";
    {
        bit7z::BitFileCompressor compressor{library, bit7z::BitFormat::Tar};
        compressor.compress({(root / L"src" / L"hello.txt").wstring()}, tarPath.wstring());
    }
    check(zipdesk::formatFromPath(tarPath.wstring()) == zipdesk::ArchiveFormat::Tar, "sample.tar format");
    const zipdesk::OperationHooks noHooks;
    const zipdesk::ArchiveInfo tarInfo = service.read(tarPath.wstring(), L"", noHooks);
    check(tarInfo.format == zipdesk::ArchiveFormat::Tar && containsPath(tarInfo, L"hello.txt", 12),
          "tar lists hello.txt");
    const fs::path tarOut = root / L"out-tar";
    zipdesk::ExtractRequest tarExtract;
    tarExtract.archivePath = tarPath.wstring();
    tarExtract.outputDir = tarOut.wstring();
    tarExtract.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(tarExtract, noHooks);
    check(readFile(findChild(tarOut, L"hello.txt")) == "hello-tar-gz", "tar extracts hello.txt");
    service.test(tarPath.wstring(), L"", noHooks);

    const fs::path gzPath = root / L"hello.txt.gz";
    {
        bit7z::BitFileCompressor compressor{library, bit7z::BitFormat::GZip};
        compressor.compress({(root / L"src" / L"hello.txt").wstring()}, gzPath.wstring());
    }
    check(zipdesk::formatFromPath(gzPath.wstring()) == zipdesk::ArchiveFormat::GZip, "sample.gz format");
    const zipdesk::ArchiveInfo gzInfo = service.read(gzPath.wstring(), L"", noHooks);
    check(gzInfo.format == zipdesk::ArchiveFormat::GZip && !gzInfo.entries.empty(), "gzip lists one member");
    const fs::path gzOut = root / L"out-gz";
    zipdesk::ExtractRequest gzExtract;
    gzExtract.archivePath = gzPath.wstring();
    gzExtract.outputDir = gzOut.wstring();
    gzExtract.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(gzExtract, noHooks);
    check(readFile(findChild(gzOut, L"hello.txt")) == "hello-tar-gz", "gzip extracts original file");

    const fs::path tarGzPath = root / L"sample.tar.gz";
    {
        bit7z::BitFileCompressor compressor{library, bit7z::BitFormat::GZip};
        compressor.compress({tarPath.wstring()}, tarGzPath.wstring());
    }
    check(zipdesk::formatFromPath(tarGzPath.wstring()) == zipdesk::ArchiveFormat::TarGz, "sample.tar.gz format");
    const zipdesk::ArchiveInfo nested = service.read(tarGzPath.wstring(), L"", noHooks);
    check(nested.format == zipdesk::ArchiveFormat::TarGz && containsPath(nested, L"hello.txt", 12),
          "tar.gz lists inner tar members");
    const fs::path nestedOut = root / L"out-targz";
    zipdesk::ExtractRequest nestedExtract;
    nestedExtract.archivePath = tarGzPath.wstring();
    nestedExtract.outputDir = nestedOut.wstring();
    nestedExtract.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(nestedExtract, noHooks);
    check(readFile(findChild(nestedOut, L"hello.txt")) == "hello-tar-gz", "tar.gz extracts inner files");

    const fs::path tgzPath = root / L"sample.tgz";
    fs::copy_file(tarGzPath, tgzPath);
    check(zipdesk::formatFromPath(tgzPath.wstring()) == zipdesk::ArchiveFormat::TarGz, "tgz extension is TarGz");
    const zipdesk::ArchiveInfo tgzInfo = service.read(tgzPath.wstring(), L"", noHooks);
    check(containsPath(tgzInfo, L"hello.txt", 12), "tgz lists inner tar members");

    bool rejectedEdit = false;
    try {
        service.append({tarPath.wstring(), {(root / L"src" / L"hello.txt").wstring()}, L"",
                        zipdesk::CompressionLevel::Normal},
                       noHooks);
    } catch (const zipdesk::ArchiveError&) {
        rejectedEdit = true;
    }
    check(rejectedEdit, "tar remains non-editable");
}

void testTarXzRead(zipdesk::ArchiveService& service, const fs::path& root) {
    fs::create_directories(root / L"src");
    writeFile(root / L"src" / L"hello.txt", "hello-tar-xz");
    bit7z::Bit7zLibrary library(exeDirectory() + L"\\7z.dll");

    const fs::path tarPath = root / L"sample.tar";
    {
        bit7z::BitFileCompressor compressor{library, bit7z::BitFormat::Tar};
        compressor.compress({(root / L"src" / L"hello.txt").wstring()}, tarPath.wstring());
    }

    const fs::path xzPath = root / L"hello.txt.xz";
    {
        bit7z::BitFileCompressor compressor{library, bit7z::BitFormat::Xz};
        compressor.compress({(root / L"src" / L"hello.txt").wstring()}, xzPath.wstring());
    }
    check(zipdesk::formatFromPath(xzPath.wstring()) == zipdesk::ArchiveFormat::Xz, "sample.xz format");
    const zipdesk::OperationHooks noHooks;
    const zipdesk::ArchiveInfo xzInfo = service.read(xzPath.wstring(), L"", noHooks);
    check(xzInfo.format == zipdesk::ArchiveFormat::Xz && !xzInfo.entries.empty(), "xz lists one member");
    const fs::path xzOut = root / L"out-xz";
    zipdesk::ExtractRequest xzExtract;
    xzExtract.archivePath = xzPath.wstring();
    xzExtract.outputDir = xzOut.wstring();
    xzExtract.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(xzExtract, noHooks);
    check(readFile(findChild(xzOut, L"hello.txt")) == "hello-tar-xz", "xz extracts original file");

    const fs::path tarXzPath = root / L"sample.tar.xz";
    {
        bit7z::BitFileCompressor compressor{library, bit7z::BitFormat::Xz};
        compressor.compress({tarPath.wstring()}, tarXzPath.wstring());
    }
    check(zipdesk::formatFromPath(tarXzPath.wstring()) == zipdesk::ArchiveFormat::TarXz, "sample.tar.xz format");
    const zipdesk::ArchiveInfo nested = service.read(tarXzPath.wstring(), L"", noHooks);
    check(nested.format == zipdesk::ArchiveFormat::TarXz && containsPath(nested, L"hello.txt", 12),
          "tar.xz lists inner tar members");
    const fs::path nestedOut = root / L"out-tarxz";
    zipdesk::ExtractRequest nestedExtract;
    nestedExtract.archivePath = tarXzPath.wstring();
    nestedExtract.outputDir = nestedOut.wstring();
    nestedExtract.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(nestedExtract, noHooks);
    check(readFile(findChild(nestedOut, L"hello.txt")) == "hello-tar-xz", "tar.xz extracts inner files");

    const fs::path txzPath = root / L"sample.txz";
    fs::copy_file(tarXzPath, txzPath);
    check(zipdesk::formatFromPath(txzPath.wstring()) == zipdesk::ArchiveFormat::TarXz, "txz extension is TarXz");
    const zipdesk::ArchiveInfo txzInfo = service.read(txzPath.wstring(), L"", noHooks);
    check(containsPath(txzInfo, L"hello.txt", 12), "txz lists inner tar members");

    bool rejectedEdit = false;
    try {
        service.append({xzPath.wstring(), {(root / L"src" / L"hello.txt").wstring()}, L"",
                        zipdesk::CompressionLevel::Normal},
                       noHooks);
    } catch (const zipdesk::ArchiveError&) {
        rejectedEdit = true;
    }
    check(rejectedEdit, "xz remains non-editable");
}


void writeMinimalIso(const fs::path& path, const std::string& content) {
    constexpr std::size_t SECTOR = 2048;
    const auto both32 = [](std::uint32_t n) {
        std::array<unsigned char, 8> out{};
        out[0] = static_cast<unsigned char>(n);
        out[1] = static_cast<unsigned char>(n >> 8);
        out[2] = static_cast<unsigned char>(n >> 16);
        out[3] = static_cast<unsigned char>(n >> 24);
        out[4] = static_cast<unsigned char>(n >> 24);
        out[5] = static_cast<unsigned char>(n >> 16);
        out[6] = static_cast<unsigned char>(n >> 8);
        out[7] = static_cast<unsigned char>(n);
        return out;
    };
    const auto both16 = [](std::uint16_t n) {
        std::array<unsigned char, 4> out{};
        out[0] = static_cast<unsigned char>(n);
        out[1] = static_cast<unsigned char>(n >> 8);
        out[2] = static_cast<unsigned char>(n >> 8);
        out[3] = static_cast<unsigned char>(n);
        return out;
    };
    const auto dirRecord = [&](std::uint32_t extent, std::uint32_t size, unsigned char flags,
                               const std::vector<unsigned char>& name) {
        std::size_t length = 33 + name.size();
        if (length % 2 == 1) ++length;
        std::vector<unsigned char> rec(length, 0);
        rec[0] = static_cast<unsigned char>(length);
        const auto e = both32(extent);
        const auto s = both32(size);
        const auto v = both16(1);
        std::copy(e.begin(), e.end(), rec.begin() + 2);
        std::copy(s.begin(), s.end(), rec.begin() + 10);
        rec[25] = flags;
        std::copy(v.begin(), v.end(), rec.begin() + 28);
        rec[32] = static_cast<unsigned char>(name.size());
        std::copy(name.begin(), name.end(), rec.begin() + 33);
        return rec;
    };
    const auto pathTable = [&](std::uint32_t extent, bool little) {
        std::vector<unsigned char> rec;
        rec.push_back(1);
        rec.push_back(0);
        if (little) {
            rec.push_back(static_cast<unsigned char>(extent));
            rec.push_back(static_cast<unsigned char>(extent >> 8));
            rec.push_back(static_cast<unsigned char>(extent >> 16));
            rec.push_back(static_cast<unsigned char>(extent >> 24));
            rec.push_back(1);
            rec.push_back(0);
        } else {
            rec.push_back(static_cast<unsigned char>(extent >> 24));
            rec.push_back(static_cast<unsigned char>(extent >> 16));
            rec.push_back(static_cast<unsigned char>(extent >> 8));
            rec.push_back(static_cast<unsigned char>(extent));
            rec.push_back(0);
            rec.push_back(1);
        }
        rec.push_back(0);
        rec.push_back(0);  // pad for odd name length
        return rec;
    };

    constexpr std::uint32_t rootExtent = 20;
    constexpr std::uint32_t pathTableL = 18;
    constexpr std::uint32_t pathTableM = 19;
    constexpr std::uint32_t fileExtent = 21;
    const std::uint32_t fileSize = static_cast<std::uint32_t>(content.size());
    const auto ptL = pathTable(rootExtent, true);
    const auto ptM = pathTable(rootExtent, false);
    auto rootDir = dirRecord(rootExtent, static_cast<std::uint32_t>(SECTOR), 0x02, {0x00});
    const auto dotdot = dirRecord(rootExtent, static_cast<std::uint32_t>(SECTOR), 0x02, {0x01});
    const std::vector<unsigned char> fileName = {'H','E','L','L','O','.','T','X','T',';','1'};
    const auto fileRec = dirRecord(fileExtent, fileSize, 0x00, fileName);
    rootDir.insert(rootDir.end(), dotdot.begin(), dotdot.end());
    rootDir.insert(rootDir.end(), fileRec.begin(), fileRec.end());
    rootDir.resize(SECTOR, 0);

    std::vector<unsigned char> pvd(SECTOR, 0);
    pvd[0] = 1;
    const char cd001[] = "CD001";
    for (int i = 0; i < 5; ++i) pvd[1 + i] = static_cast<unsigned char>(cd001[i]);
    pvd[6] = 1;
    const char sys[] = "QINGXIA";
    for (int i = 0; i < 7; ++i) pvd[8 + i] = static_cast<unsigned char>(sys[i]);
    for (int i = 7; i < 32; ++i) pvd[8 + i] = ' ';
    for (int i = 0; i < 32; ++i) pvd[40 + i] = ' ';
    const auto volSize = both32(22);
    std::copy(volSize.begin(), volSize.end(), pvd.begin() + 80);
    const auto one16 = both16(1);
    std::copy(one16.begin(), one16.end(), pvd.begin() + 120);
    std::copy(one16.begin(), one16.end(), pvd.begin() + 124);
    const auto block = both16(static_cast<std::uint16_t>(SECTOR));
    std::copy(block.begin(), block.end(), pvd.begin() + 128);
    const auto pts = both32(static_cast<std::uint32_t>(ptL.size()));
    std::copy(pts.begin(), pts.end(), pvd.begin() + 132);
    pvd[140] = static_cast<unsigned char>(pathTableL);
    pvd[141] = static_cast<unsigned char>(pathTableL >> 8);
    pvd[142] = static_cast<unsigned char>(pathTableL >> 16);
    pvd[143] = static_cast<unsigned char>(pathTableL >> 24);
    pvd[148] = static_cast<unsigned char>(pathTableM >> 24);
    pvd[149] = static_cast<unsigned char>(pathTableM >> 16);
    pvd[150] = static_cast<unsigned char>(pathTableM >> 8);
    pvd[151] = static_cast<unsigned char>(pathTableM);
    const auto rootRec = dirRecord(rootExtent, static_cast<std::uint32_t>(SECTOR), 0x02, {0x00});
    std::copy(rootRec.begin(), rootRec.end(), pvd.begin() + 156);
    for (int i = 190; i < 318; ++i) pvd[i] = ' ';
    for (int base : {318, 446, 574}) {
        for (int i = 0; i < 7; ++i) pvd[base + i] = static_cast<unsigned char>(sys[i]);
        for (int i = 7; i < 128; ++i) pvd[base + i] = ' ';
    }
    for (int i = 702; i < 813; ++i) pvd[i] = ' ';
    pvd[881] = 1;

    std::vector<unsigned char> term(SECTOR, 0);
    term[0] = 255;
    for (int i = 0; i < 5; ++i) term[1 + i] = static_cast<unsigned char>(cd001[i]);
    term[6] = 1;

    std::vector<unsigned char> img(22 * SECTOR, 0);
    std::copy(pvd.begin(), pvd.end(), img.begin() + 16 * SECTOR);
    std::copy(term.begin(), term.end(), img.begin() + 17 * SECTOR);
    std::copy(ptL.begin(), ptL.end(), img.begin() + 18 * SECTOR);
    std::copy(ptM.begin(), ptM.end(), img.begin() + 19 * SECTOR);
    std::copy(rootDir.begin(), rootDir.end(), img.begin() + 20 * SECTOR);
    std::copy(content.begin(), content.end(), img.begin() + 21 * SECTOR);

    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
}

void testIsoRead(zipdesk::ArchiveService& service, const fs::path& root) {
    const fs::path isoPath = root / L"sample.iso";
    writeMinimalIso(isoPath, "hello-iso\n");
    check(zipdesk::formatFromPath(isoPath.wstring()) == zipdesk::ArchiveFormat::Iso, "sample.iso format");
    const zipdesk::OperationHooks noHooks;
    const zipdesk::ArchiveInfo info = service.read(isoPath.wstring(), L"", noHooks);
    check(info.format == zipdesk::ArchiveFormat::Iso && containsPath(info, L"HELLO.TXT", 10),
          "iso lists HELLO.TXT");
    const fs::path outDir = root / L"out-iso";
    zipdesk::ExtractRequest extract;
    extract.archivePath = isoPath.wstring();
    extract.outputDir = outDir.wstring();
    extract.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(extract, noHooks);
    check(readFile(findChild(outDir, L"HELLO.TXT")) == "hello-iso\n", "iso extracts HELLO.TXT");

    bool rejectedEdit = false;
    try {
        service.append({isoPath.wstring(), {(root / L"dummy.txt").wstring()}, L"",
                        zipdesk::CompressionLevel::Normal},
                       noHooks);
    } catch (const zipdesk::ArchiveError&) {
        rejectedEdit = true;
    }
    check(rejectedEdit, "iso remains non-editable");
}



void testSplitVolumeRead(zipdesk::ArchiveService& service, const fs::path& root) {
    fs::create_directories(root);
    {
        std::ofstream output(root / L"payload.bin", std::ios::binary);
        for (int i = 0; i < 64 * 1024; ++i) output.put(static_cast<char>(i & 0xff));
    }
    const fs::path volumeOutput = root / L"pack.zip";
    zipdesk::CompressRequest compress;
    compress.inputs = {(root / L"payload.bin").wstring()};
    compress.outputPath = volumeOutput.wstring();
    compress.format = zipdesk::ArchiveFormat::Zip;
    compress.level = zipdesk::CompressionLevel::Store;
    compress.volumeSize = 16 * 1024;
    service.compress(compress, {});
    const fs::path first = fs::path(volumeOutput.wstring() + L".001");
    const fs::path second = fs::path(volumeOutput.wstring() + L".002");
    check(fs::exists(first), "multi-volume created .001");
    check(fs::exists(second), "multi-volume created .002");
    check(zipdesk::formatFromPath(first.wstring()) == zipdesk::ArchiveFormat::SplitZip,
          "first volume is SplitZip");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::SplitZip), "SplitZip read-only");
    check(zipdesk::resolveVolumeOpenPath(second.wstring()) == first.wstring(),
          ".002 resolves to .001");

    zipdesk::OperationHooks noHooks;
    const zipdesk::ArchiveInfo info = service.read(first.wstring(), L"", noHooks);
    check(info.format == zipdesk::ArchiveFormat::SplitZip && containsPath(info, L"payload.bin", 64ull * 1024),
          "split volume lists payload.bin");
    // Opening a later volume should redirect to the first.
    const zipdesk::ArchiveInfo viaSecond = service.read(second.wstring(), L"", noHooks);
    check(viaSecond.format == zipdesk::ArchiveFormat::SplitZip &&
              viaSecond.path == first.wstring() &&
              containsPath(viaSecond, L"payload.bin", 64ull * 1024),
          "opening .002 reads via .001");

    const fs::path outDir = root / L"out-split";
    zipdesk::ExtractRequest extract;
    extract.archivePath = first.wstring();
    extract.outputDir = outDir.wstring();
    extract.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(extract, noHooks);
    {
        const auto extracted = findChild(outDir, L"payload.bin");
        check(fs::file_size(extracted) == 64ull * 1024, "split volume extracts payload size");
    }
    service.test(first.wstring(), L"", noHooks);

    bool rejectedEdit = false;
    try {
        service.append({first.wstring(), {(root / L"payload.bin").wstring()}, L"",
                        zipdesk::CompressionLevel::Store}, noHooks);
    } catch (const zipdesk::ArchiveError&) {
        rejectedEdit = true;
    }
    check(rejectedEdit, "split volume remains non-editable");
}

void testZipxRead(zipdesk::ArchiveService& service, const fs::path& root) {
    fs::create_directories(root);
    writeFile(root / L"hello.txt", "hello-zipx\n");
    zipdesk::CompressRequest compress;
    compress.inputs = {(root / L"hello.txt").wstring()};
    compress.outputPath = (root / L"sample.zip").wstring();
    compress.format = zipdesk::ArchiveFormat::Zip;
    compress.level = zipdesk::CompressionLevel::Normal;
    const zipdesk::OperationHooks noHooks;
    service.compress(compress, noHooks);
    const fs::path zipxPath = root / L"sample.zipx";
    fs::rename(root / L"sample.zip", zipxPath);

    check(zipdesk::formatFromPath(zipxPath.wstring()) == zipdesk::ArchiveFormat::Zipx, "sample.zipx format");
    check(zipdesk::isReadOnlyFormat(zipdesk::ArchiveFormat::Zipx), "zipx format is read-only");
    const zipdesk::ArchiveInfo info = service.read(zipxPath.wstring(), L"", noHooks);
    check(info.format == zipdesk::ArchiveFormat::Zipx && containsPath(info, L"hello.txt", 11),
          "zipx lists hello.txt");

    const fs::path outDir = root / L"out-zipx";
    zipdesk::ExtractRequest extract;
    extract.archivePath = zipxPath.wstring();
    extract.outputDir = outDir.wstring();
    extract.overwrite = zipdesk::OverwritePolicy::Overwrite;
    service.extract(extract, noHooks);
    check(readFile(findChild(outDir, L"hello.txt")) == "hello-zipx\n", "zipx extracts hello.txt");

    service.test(zipxPath.wstring(), L"", noHooks);

    bool rejectedEdit = false;
    try {
        service.append({zipxPath.wstring(), {(root / L"hello.txt").wstring()}, L"",
                        zipdesk::CompressionLevel::Normal},
                       noHooks);
    } catch (const zipdesk::ArchiveError&) {
        rejectedEdit = true;
    }
    check(rejectedEdit, "zipx remains non-editable");

    bool rejectedCreate = false;
    try {
        zipdesk::CompressRequest bad;
        bad.inputs = {(root / L"hello.txt").wstring()};
        bad.outputPath = (root / L"created.zipx").wstring();
        bad.format = zipdesk::ArchiveFormat::Zipx;
        service.compress(bad, noHooks);
    } catch (const zipdesk::ArchiveError&) {
        rejectedCreate = true;
    }
    check(rejectedCreate, "zipx cannot be created");
}

int main() {
    testClassicContextMenuRegistry();
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
        testReadItemSizeLimit(service, root / L"read-limit");
        testTarGzipRead(service, root / L"tar-gz");
        testTarXzRead(service, root / L"tar-xz");
        testIsoRead(service, root / L"iso");
        testZipxRead(service, root / L"zipx");
        testSplitVolumeRead(service, root / L"split-vol");
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
