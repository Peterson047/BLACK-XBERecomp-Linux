#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "kernel/exports.h"

namespace fs = std::filesystem;

namespace kernel {
uint8_t* g_base = nullptr;

static std::map<uint32_t, KernelFn> g_exports_by_ord;
static std::map<std::string, KernelFn> g_exports_by_name;

void RegisterExport(uint32_t ord, const char* name, KernelFn fn) {
    g_exports_by_ord[ord] = fn;
    if (name) g_exports_by_name[name] = fn;
}

static std::mutex g_handles_lock;
static std::map<uint32_t, ObjectRef> g_handles;
static uint32_t g_next_handle = 100;

ObjectRef FromHandle(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_handles_lock);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

uint32_t NewHandle(std::shared_ptr<Object> obj) {
    std::lock_guard<std::mutex> lock(g_handles_lock);
    uint32_t h = g_next_handle++;
    g_handles[h] = obj;
    return h;
}

bool CloseHandle(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_handles_lock);
    return g_handles.erase(handle) > 0;
}

int32_t ResetEvent(Object&) { return 0; }
int32_t SetEvent(Object&) { return 0; }
Thread* Current() { return nullptr; }
void QueueApc(Thread&, uint32_t, uint32_t, uint32_t, uint32_t) {}
}

struct TestReport {
    int total_tests = 0;
    int passed_tests = 0;
    int failed_tests = 0;
    std::vector<std::string> failures;

    void Assert(bool condition, const std::string& desc) {
        total_tests++;
        if (condition) {
            passed_tests++;
            std::cout << "  [PASS] " << desc << std::endl;
        } else {
            failed_tests++;
            failures.push_back(desc);
            std::cerr << "  [FAIL] " << desc << std::endl;
        }
    }
};

static uint32_t CallKernelExport(uint32_t ord, const std::vector<uint32_t>& args) {
    auto it = kernel::g_exports_by_ord.find(ord);
    if (it == kernel::g_exports_by_ord.end()) {
        std::cerr << "Export ord " << ord << " not found!" << std::endl;
        return 0xC0000002; // kNotImplemented
    }
    X86Context ctx{};
    // Setup a 64KB stack in guest memory at 0x100000
    uint32_t stack_base = 0x110000;
    ctx.esp = stack_base;
    // Push return address
    kernel::W32(ctx.esp, 0xDEADBEEF);
    // Push arguments (stdcall)
    for (size_t i = 0; i < args.size(); ++i) {
        kernel::W32(ctx.esp + 4 + uint32_t(i) * 4, args[i]);
    }
    it->second(ctx, kernel::g_base);
    return ctx.eax;
}

static uint32_t SetupAnsiString(uint32_t va_str, uint32_t va_buf, const std::string& s) {
    uint16_t len = uint16_t(s.size());
    kernel::W16(va_str + 0, len);
    kernel::W16(va_str + 2, len + 1);
    kernel::W32(va_str + 4, va_buf);
    std::memcpy(kernel::g_base + va_buf, s.data(), s.size());
    kernel::g_base[va_buf + s.size()] = 0;
    return va_str;
}

static uint32_t SetupObjectAttributes(uint32_t va_attrs, uint32_t root_handle, uint32_t va_ansi_str) {
    kernel::W32(va_attrs + 0, root_handle);
    kernel::W32(va_attrs + 4, va_ansi_str);
    return va_attrs;
}

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << "ADVERSARIAL CHALLENGER: Directory Safety & Exception Harness" << std::endl;
    std::cout << "============================================================" << std::endl;

    TestReport report;

    // Allocate 16MB guest memory
    kernel::g_base = new uint8_t[0x1000000]();

    // Mount kernel filesystem
    kernel::MountDefaults("assets", "assets/hdd");
    auto disc = std::make_unique<kernel::Disc>();
    std::string err;
    bool iso_ok = disc->Open("assets/Black.iso", err);
    report.Assert(iso_ok, "0.1 assets/Black.iso opened successfully: " + err);
    if (!iso_ok) {
        std::cerr << "CRITICAL: Cannot open assets/Black.iso. Aborting." << std::endl;
        return 1;
    }
    kernel::MountDisc(std::move(disc));
    report.Assert(kernel::MountedDisc() != nullptr, "0.2 MountedDisc is valid");

    // =========================================================================
    // SUITE 1: Directory Queries on Host Filesystem via ResolvePath
    // =========================================================================
    std::cout << "\n--- Suite 1: Directory Queries on Host Filesystem ---" << std::endl;
    {
        // 1.1 Device root
        std::string p_d = kernel::ResolvePath("D:", 0);
        report.Assert(!p_d.empty() && fs::is_directory(p_d), "1.1 'D:' resolves to host directory: " + p_d);

        // 1.2 Device root with slash
        std::string p_ds = kernel::ResolvePath("D:\\", 0);
        report.Assert(!p_ds.empty() && fs::is_directory(p_ds), "1.2 'D:\\' resolves to host directory: " + p_ds);

        // 1.3 D:\Levels
        std::string p_lv = kernel::ResolvePath("D:\\Levels", 0);
        report.Assert(!p_lv.empty() && fs::is_directory(p_lv), "1.3 'D:\\Levels' resolves to host directory: " + p_lv);

        // 1.4 D:\Levels\ with trailing slash
        std::string p_lvs = kernel::ResolvePath("D:\\Levels\\", 0);
        report.Assert(!p_lvs.empty() && fs::is_directory(p_lvs), "1.4 'D:\\Levels\\' resolves to host directory: " + p_lvs);

        // 1.5 D:\Levels\Level_00 (must redirect to clean level_mp00 directory)
        std::string p_l0 = kernel::ResolvePath("D:\\Levels\\Level_00", 0);
        report.Assert(!p_l0.empty() && fs::is_directory(p_l0) && p_l0.find("level_mp00") != std::string::npos,
                      "1.5 'D:\\Levels\\Level_00' redirected to level_mp00 directory: " + p_l0);

        // 1.6 D:\Levels\Level_00\ with trailing slash
        std::string p_l0s = kernel::ResolvePath("D:\\Levels\\Level_00\\", 0);
        report.Assert(!p_l0s.empty() && fs::is_directory(p_l0s) && p_l0s.find("level_mp00") != std::string::npos,
                      "1.6 'D:\\Levels\\Level_00\\' redirected to level_mp00 directory: " + p_l0s);

        // 1.7 D:\Levels\level_mp00
        std::string p_mp = kernel::ResolvePath("D:\\Levels\\level_mp00", 0);
        report.Assert(!p_mp.empty() && fs::is_directory(p_mp) && p_mp.find("level_mp00") != std::string::npos,
                      "1.7 'D:\\Levels\\level_mp00' resolves to level_mp00 directory: " + p_mp);

        // 1.8 Subdirectories: stg_0001
        std::string p_stg = kernel::ResolvePath("D:\\Levels\\Level_00\\stg_0001", 0);
        report.Assert(!p_stg.empty() && fs::is_directory(p_stg), "1.8 Subdirectory stg_0001 is directory: " + p_stg);

        // 1.9 Subdirectories: fpguns
        std::string p_fp = kernel::ResolvePath("D:\\Levels\\Level_00\\fpguns", 0);
        report.Assert(!p_fp.empty() && fs::is_directory(p_fp), "1.9 Subdirectory fpguns is directory: " + p_fp);

        // 1.10 Relative dot segments on directory
        std::string p_dot = kernel::ResolvePath("D:\\Levels\\.\\Level_00\\.", 0);
        report.Assert(!p_dot.empty() && fs::is_directory(p_dot) && p_dot.find("level_mp00") != std::string::npos,
                      "1.10 Dot directory segments resolve to level_mp00: " + p_dot);

        // 1.11 Relative dot-dot segments on directory
        std::string p_dotdot = kernel::ResolvePath("D:\\Levels\\Level_01\\..\\Level_00", 0);
        report.Assert(!p_dotdot.empty() && fs::is_directory(p_dotdot) && p_dotdot.find("level_mp00") != std::string::npos,
                      "1.11 Dot-dot directory segments resolve to level_mp00: " + p_dotdot);
    }

    // =========================================================================
    // SUITE 2: Adversarial Directory Read Safety via ReadGameFile
    // =========================================================================
    std::cout << "\n--- Suite 2: Adversarial Directory Read Safety (EISDIR / std::__ios_failure) ---" << std::endl;
    {
        auto test_dir_read = [&](const std::string& path, const std::string& desc) {
            std::vector<uint8_t> data;
            bool threw = false;
            std::string err_msg;
            bool ret = false;
            try {
                ret = kernel::ReadGameFile(path, data);
            } catch (const std::exception& e) {
                threw = true;
                err_msg = e.what();
            } catch (...) {
                threw = true;
                err_msg = "Unknown exception";
            }
            report.Assert(!threw && !ret && data.empty(),
                          desc + (threw ? (" [CRITICAL: THREW EXCEPTION: " + err_msg + "]") : " -> safely returned false"));
        };

        // 2.1 Host directory: D:\Levels
        test_dir_read("D:\\Levels", "2.1 ReadGameFile('D:\\Levels')");

        // 2.2 Host directory with slash: D:\Levels\ 
        test_dir_read("D:\\Levels\\", "2.2 ReadGameFile('D:\\Levels\\')");

        // 2.3 Host level directory: D:\Levels\Level_00
        test_dir_read("D:\\Levels\\Level_00", "2.3 ReadGameFile('D:\\Levels\\Level_00')");

        // 2.4 Host level directory with slash: D:\Levels\Level_00\ 
        test_dir_read("D:\\Levels\\Level_00\\", "2.4 ReadGameFile('D:\\Levels\\Level_00\\')");

        // 2.5 Host level_mp00 directory: D:\Levels\level_mp00
        test_dir_read("D:\\Levels\\level_mp00", "2.5 ReadGameFile('D:\\Levels\\level_mp00')");

        // 2.6 Host level_mp00 subdirectory: D:\Levels\level_mp00\stg_0001
        test_dir_read("D:\\Levels\\level_mp00\\stg_0001", "2.6 ReadGameFile('D:\\Levels\\level_mp00\\stg_0001')");

        // 2.7 Host level_mp00 subdirectory: D:\Levels\level_mp00\fpguns
        test_dir_read("D:\\Levels\\level_mp00\\fpguns", "2.7 ReadGameFile('D:\\Levels\\level_mp00\\fpguns')");

        // 2.8 Host device root: D:
        test_dir_read("D:", "2.8 ReadGameFile('D:')");

        // 2.9 Host device root with slash: D:\ 
        test_dir_read("D:\\", "2.9 ReadGameFile('D:\\')");

        // 2.10 Host directory with dot: D:\Levels\.
        test_dir_read("D:\\Levels\\.", "2.10 ReadGameFile('D:\\Levels\\.')");

        // 2.11 Host directory with dot: D:\Levels\Level_00\.
        test_dir_read("D:\\Levels\\Level_00\\.", "2.11 ReadGameFile('D:\\Levels\\Level_00\\.')");

        // 2.12 Host directory with dot-dot: D:\Levels\Level_00\..
        test_dir_read("D:\\Levels\\Level_00\\..", "2.12 ReadGameFile('D:\\Levels\\Level_00\\..')");

        // 2.13 ISO directory: D:\videos
        test_dir_read("D:\\videos", "2.13 ReadGameFile('D:\\videos') [ISO directory]");

        // 2.14 ISO directory with slash: D:\videos\ 
        test_dir_read("D:\\videos\\", "2.14 ReadGameFile('D:\\videos\\') [ISO directory]");

        // 2.15 ISO directory: D:\Levels\Level_01
        test_dir_read("D:\\Levels\\Level_01", "2.15 ReadGameFile('D:\\Levels\\Level_01') [ISO directory]");

        // 2.16 ISO directory: D:\Levels\Level_02
        test_dir_read("D:\\Levels\\Level_02", "2.16 ReadGameFile('D:\\Levels\\Level_02') [ISO directory]");

        // 2.17 Non-existent directory: D:\NonExistentDirectory
        test_dir_read("D:\\NonExistentDirectory", "2.17 ReadGameFile('D:\\NonExistentDirectory')");

        // 2.18 Non-existent subdirectory in Level_00: D:\Levels\Level_00\FakeDir
        test_dir_read("D:\\Levels\\Level_00\\FakeDir", "2.18 ReadGameFile('D:\\Levels\\Level_00\\FakeDir')");

        // 2.19 Non-existent level directory: D:\Levels\Level_99
        test_dir_read("D:\\Levels\\Level_99", "2.19 ReadGameFile('D:\\Levels\\Level_99')");
    }

    // =========================================================================
    // SUITE 3: Hostile String & Boundary Injections
    // =========================================================================
    std::cout << "\n--- Suite 3: Hostile String & Boundary Injections ---" << std::endl;
    {
        auto test_boundary_read = [&](const std::string& path, const std::string& desc) {
            std::vector<uint8_t> data;
            bool threw = false;
            std::string err_msg;
            bool ret = false;
            try {
                ret = kernel::ReadGameFile(path, data);
            } catch (const std::exception& e) {
                threw = true;
                err_msg = e.what();
            } catch (...) {
                threw = true;
                err_msg = "Unknown exception";
            }
            report.Assert(!threw && !ret && data.empty(),
                          desc + (threw ? (" [CRITICAL: THREW EXCEPTION: " + err_msg + "]") : " -> safely returned false"));
        };

        // 3.1 Empty string
        test_boundary_read("", "3.1 Empty string ''");

        // 3.2 Whitespace only
        test_boundary_read("   ", "3.2 Whitespace '   '");

        // 3.3 Root slashes
        test_boundary_read("/", "3.3 Forward slash '/'");
        test_boundary_read("//", "3.4 Double forward slash '//'");
        test_boundary_read("\\\\", "3.5 Double backslash '\\\\'");

        // 3.4 Drive with whitespace
        test_boundary_read("D:\\   ", "3.6 Drive with whitespace 'D:\\   '");

        // 3.5 Path traversal past root
        test_boundary_read("D:\\..\\..\\..\\..\\..\\etc\\passwd", "3.7 Path traversal 'D:\\..\\..\\..\\..\\..\\etc\\passwd'");
        test_boundary_read("D:\\Levels\\Level_00\\..\\..\\..\\..\\..\\etc\\shadow", "3.8 Path traversal 'D:\\Levels\\Level_00\\..\\..\\..\\..\\..\\etc\\shadow'");

        // 3.6 Illegal colons in path
        test_boundary_read("D:\\Levels\\Level_00:stream", "3.9 Colon in path 'D:\\Levels\\Level_00:stream'");
        test_boundary_read("D:::Levels", "3.10 Consecutive colons 'D:::Levels'");

        // 3.7 Embedded null bytes in std::string
        std::string null_mid("D:\\Levels\0\\Level_00", 20);
        test_boundary_read(null_mid, "3.11 Embedded null byte in directory segment");

        std::string null_lead("\0D:\\Levels", 10);
        test_boundary_read(null_lead, "3.12 Leading null byte in path");

        std::string null_file("D:\\Levels\\Level_00\\LevelDat\0.bin", 33);
        test_boundary_read(null_file, "3.13 Embedded null byte in filename");

        // 3.8 Hostile shell injection characters
        test_boundary_read("D:\\Levels\\Level_00;rm -rf /", "3.14 Shell semicolon 'D:\\Levels\\Level_00;rm -rf /'");
        test_boundary_read("D:\\Levels\\Level_00|cat", "3.15 Shell pipe 'D:\\Levels\\Level_00|cat'");
        test_boundary_read("D:\\Levels\\Level_00&echo", "3.16 Shell ampersand 'D:\\Levels\\Level_00&echo'");
        test_boundary_read("D:\\Levels\\$HOME", "3.17 Shell env variable 'D:\\Levels\\$HOME'");

        // 3.9 Wildcards in ReadGameFile
        test_boundary_read("D:\\Levels\\*", "3.18 Wildcard 'D:\\Levels\\*'");
        test_boundary_read("D:\\Levels\\Level_*", "3.19 Wildcard 'D:\\Levels\\Level_*'");
        test_boundary_read("D:\\Levels\\?evel_00", "3.20 Wildcard 'D:\\Levels\\?evel_00'");

        // 3.10 Special formatting / control chars
        test_boundary_read("D:\\Levels\\Level_00\n", "3.21 Newline in path");
        test_boundary_read("D:\\Levels\\Level_00\t", "3.22 Tab in path");
        test_boundary_read("D:\\Levels\\Level_00\r\n", "3.23 CRLF in path");

        // 3.11 Non-ASCII / UTF-8
        test_boundary_read("D:\\Levels\\Level_00\\テスト", "3.24 UTF-8 Japanese characters");
        test_boundary_read("D:\\Levels\\Level_00\\diretório_limpo", "3.25 Latin-1 accented characters");

        // 3.12 Deeply nested path (300 segments)
        std::string deep = "D:\\Levels\\Level_00";
        for (int i = 0; i < 300; i++) deep += "\\sub";
        deep += "\\file.bin";
        test_boundary_read(deep, "3.26 Deeply nested path (300 segments)");

        // 3.13 Extremely long component (4,096 chars)
        std::string long_comp = "D:\\Levels\\Level_00\\" + std::string(4096, 'A') + ".bin";
        test_boundary_read(long_comp, "3.27 Extremely long path component (4,096 chars)");

        // 3.14 500 consecutive '..'
        std::string many_dots = "D:\\Levels\\Level_00";
        for (int i = 0; i < 500; i++) many_dots += "\\..";
        test_boundary_read(many_dots, "3.28 500 consecutive '..' segments");

        // 3.15 500 consecutive '.'
        std::string single_dots = "D:\\Levels\\Level_00";
        for (int i = 0; i < 500; i++) single_dots += "\\.";
        test_boundary_read(single_dots, "3.29 500 consecutive '.' segments");
    }

    // =========================================================================
    // SUITE 4: Direct Xbox Kernel NT File APIs on Directories
    // =========================================================================
    std::cout << "\n--- Suite 4: Direct Xbox Kernel NT File APIs on Directories ---" << std::endl;
    {
        uint32_t va_handle_out = 0x20000;
        uint32_t va_iosb = 0x20010;
        uint32_t va_attrs = 0x20030;
        uint32_t va_ansi = 0x20050;
        uint32_t va_buf = 0x20100;
        uint32_t va_read_buf = 0x30000;

        // 4.1 NtOpenFile with FILE_NON_DIRECTORY_FILE (options = 0x40) on D:\Levels
        // Must reject opening a directory with kFileIsADirectory (0xC00000BA)
        SetupAnsiString(va_ansi, va_buf, "\\??\\D:\\Levels");
        SetupObjectAttributes(va_attrs, 0, va_ansi);
        uint32_t status_4_1 = CallKernelExport(202, {va_handle_out, 0, va_attrs, va_iosb, 0, 0x40});
        report.Assert(status_4_1 == kernel::kFileIsADirectory,
                      "4.1 NtOpenFile(D:\\Levels, FILE_NON_DIRECTORY_FILE) returned kFileIsADirectory (0xC00000BA)");

        // 4.2 NtOpenFile with FILE_NON_DIRECTORY_FILE (options = 0x40) on D:\Levels\Level_00
        SetupAnsiString(va_ansi, va_buf, "\\??\\D:\\Levels\\Level_00");
        SetupObjectAttributes(va_attrs, 0, va_ansi);
        uint32_t status_4_2 = CallKernelExport(202, {va_handle_out, 0, va_attrs, va_iosb, 0, 0x40});
        report.Assert(status_4_2 == kernel::kFileIsADirectory,
                      "4.2 NtOpenFile(D:\\Levels\\Level_00, FILE_NON_DIRECTORY_FILE) returned kFileIsADirectory (0xC00000BA)");

        // 4.3 NtOpenFile with FILE_NON_DIRECTORY_FILE on ISO directory D:\videos
        SetupAnsiString(va_ansi, va_buf, "\\??\\D:\\videos");
        SetupObjectAttributes(va_attrs, 0, va_ansi);
        uint32_t status_4_3 = CallKernelExport(202, {va_handle_out, 0, va_attrs, va_iosb, 0, 0x40});
        report.Assert(status_4_3 == kernel::kFileIsADirectory,
                      "4.3 NtOpenFile(D:\\videos, FILE_NON_DIRECTORY_FILE) on ISO returned kFileIsADirectory (0xC00000BA)");

        // 4.4 NtOpenFile with FILE_DIRECTORY_FILE (options = 0x01) on D:\Levels
        // Must succeed with kSuccess (0x0) and produce a valid handle
        SetupAnsiString(va_ansi, va_buf, "\\??\\D:\\Levels");
        SetupObjectAttributes(va_attrs, 0, va_ansi);
        uint32_t status_4_4 = CallKernelExport(202, {va_handle_out, 0, va_attrs, va_iosb, 0, 0x01});
        uint32_t h_dir = kernel::R32(va_handle_out);
        report.Assert(status_4_4 == kernel::kSuccess && h_dir != 0,
                      "4.4 NtOpenFile(D:\\Levels, FILE_DIRECTORY_FILE) succeeded with handle " + std::to_string(h_dir));

        // 4.5 NtReadFile on directory handle
        // Must fail with kInvalidHandle (0xC0000008) without reading or throwing EISDIR
        uint32_t status_4_5 = CallKernelExport(219, {h_dir, 0, 0, 0, va_iosb, va_read_buf, 512, 0});
        report.Assert(status_4_5 == kernel::kInvalidHandle,
                      "4.5 NtReadFile on directory handle safely returned kInvalidHandle (0xC0000008)");

        // 4.6 NtQueryDirectoryFile on directory handle
        // Must succeed and enumerate files/directories
        uint32_t va_dir_info = 0x40000;
        uint32_t status_4_6 = CallKernelExport(207, {h_dir, 0, 0, 0, va_iosb, va_dir_info, 1024, 1, 0, 1});
        report.Assert(status_4_6 == kernel::kSuccess,
                      "4.6 NtQueryDirectoryFile on D:\\Levels succeeded (kSuccess)");
        if (status_4_6 == kernel::kSuccess) {
            uint32_t name_len = kernel::R32(va_dir_info + 0x3C);
            std::string item_name(reinterpret_cast<char*>(kernel::g_base + va_dir_info + 0x40), name_len);
            report.Assert(!item_name.empty(), "4.7 Enumerated directory entry: '" + item_name + "'");
        }

        // 4.7 NtQueryFullAttributesFile on D:\Levels\Level_00
        // Must report FILE_ATTRIBUTE_DIRECTORY (0x10)
        uint32_t va_net_info = 0x50000;
        SetupAnsiString(va_ansi, va_buf, "\\??\\D:\\Levels\\Level_00");
        SetupObjectAttributes(va_attrs, 0, va_ansi);
        uint32_t status_4_8 = CallKernelExport(210, {va_attrs, va_net_info});
        uint32_t attrs = kernel::R32(va_net_info + 0x30);
        report.Assert(status_4_8 == kernel::kSuccess && (attrs & 0x10) != 0,
                      "4.8 NtQueryFullAttributesFile(Level_00) returned FILE_ATTRIBUTE_DIRECTORY (attrs: 0x" +
                      std::to_string(attrs) + ")");

        // 4.8 NtQueryFullAttributesFile on LevelDat.bin (file)
        // Must report FILE_ATTRIBUTE_NORMAL (0x80), NOT directory
        SetupAnsiString(va_ansi, va_buf, "\\??\\D:\\Levels\\Level_00\\LevelDat.bin");
        SetupObjectAttributes(va_attrs, 0, va_ansi);
        uint32_t status_4_9 = CallKernelExport(210, {va_attrs, va_net_info});
        uint32_t f_attrs = kernel::R32(va_net_info + 0x30);
        report.Assert(status_4_9 == kernel::kSuccess && (f_attrs & 0x80) != 0 && (f_attrs & 0x10) == 0,
                      "4.9 NtQueryFullAttributesFile(LevelDat.bin) returned FILE_ATTRIBUTE_NORMAL (attrs: 0x" +
                      std::to_string(f_attrs) + ")");
    }

    // =========================================================================
    // SUITE 5: High-Concurrency Multithreaded Chaos Stress Harness
    // =========================================================================
    std::cout << "\n--- Suite 5: High-Concurrency Multithreaded Chaos Stress ---" << std::endl;
    {
        constexpr int kNumThreads = 16;
        constexpr int kOpsPerThread = 500;
        std::vector<std::thread> workers;
        std::atomic<int> success_count{0};
        std::atomic<int> uncaught_exceptions{0};
        std::atomic<int> read_game_file_errors{0};

        std::cout << "  ... Spawning " << kNumThreads << " threads x " << kOpsPerThread
                  << " ops (" << (kNumThreads * kOpsPerThread) << " total concurrent operations) ..." << std::endl;

        auto t0 = std::chrono::steady_clock::now();

        for (int tid = 0; tid < kNumThreads; ++tid) {
            workers.emplace_back([tid, &success_count, &uncaught_exceptions, &read_game_file_errors]() {
                for (int i = 0; i < kOpsPerThread; ++i) {
                    try {
                        int op = (tid * 37 + i) % 8;
                        std::vector<uint8_t> data;
                        switch (op) {
                            case 0: {
                                // Host directory read
                                bool ok = kernel::ReadGameFile("D:\\Levels\\Level_00", data);
                                if (ok || !data.empty()) read_game_file_errors++;
                                else success_count++;
                                break;
                            }
                            case 1: {
                                // Valid clean file read
                                bool ok = kernel::ReadGameFile("D:\\Levels\\Level_00\\LevelDat.bin", data);
                                if (ok && data.size() == 711088) success_count++;
                                else read_game_file_errors++;
                                break;
                            }
                            case 2: {
                                // Host level_mp00 directory read
                                bool ok = kernel::ReadGameFile("D:\\Levels\\level_mp00", data);
                                if (ok || !data.empty()) read_game_file_errors++;
                                else success_count++;
                                break;
                            }
                            case 3: {
                                // Clean speech dummy read (104 bytes)
                                bool ok = kernel::ReadGameFile("D:\\Levels\\Level_00\\speech.slb", data);
                                if (ok && data.size() == 104) success_count++;
                                else read_game_file_errors++;
                                break;
                            }
                            case 4: {
                                // ISO directory read
                                bool ok = kernel::ReadGameFile("D:\\videos", data);
                                if (ok || !data.empty()) read_game_file_errors++;
                                else success_count++;
                                break;
                            }
                            case 5: {
                                // Non-existent directory
                                bool ok = kernel::ReadGameFile("D:\\NonExistent_ChaosDir_12345", data);
                                if (ok || !data.empty()) read_game_file_errors++;
                                else success_count++;
                                break;
                            }
                            case 6: {
                                // Traversal probe
                                bool ok = kernel::ReadGameFile("D:\\Levels\\Level_00\\..\\..\\..\\..\\etc\\passwd", data);
                                if (ok || !data.empty()) read_game_file_errors++;
                                else success_count++;
                                break;
                            }
                            case 7: {
                                // Null byte injection
                                std::string s("D:\\Levels\0\\Level_00", 20);
                                bool ok = kernel::ReadGameFile(s, data);
                                if (ok || !data.empty()) read_game_file_errors++;
                                else success_count++;
                                break;
                            }
                        }
                    } catch (const std::exception& e) {
                        uncaught_exceptions++;
                        std::cerr << "THREAD " << tid << " EXCEPTION: " << e.what() << std::endl;
                    } catch (...) {
                        uncaught_exceptions++;
                        std::cerr << "THREAD " << tid << " UNKNOWN EXCEPTION!" << std::endl;
                    }
                }
            });
        }

        for (auto& w : workers) w.join();
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        report.Assert(uncaught_exceptions == 0,
                      "5.1 Zero uncaught exceptions during 8,000 concurrent operations (actual: " +
                      std::to_string(uncaught_exceptions.load()) + ")");
        report.Assert(read_game_file_errors == 0,
                      "5.2 Zero file operation logic errors during 8,000 concurrent operations (actual: " +
                      std::to_string(read_game_file_errors.load()) + ")");
        report.Assert(success_count == kNumThreads * kOpsPerThread,
                      "5.3 All 8,000 concurrent operations completed successfully in " +
                      std::to_string(ms) + " ms");
    }

    // =========================================================================
    // VERDICT
    // =========================================================================
    std::cout << "\n============================================================" << std::endl;
    std::cout << "CHALLENGER STRESS HARNESS SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "Total Assertions: " << report.total_tests << std::endl;
    std::cout << "Passed:           " << report.passed_tests << std::endl;
    std::cout << "Failed:           " << report.failed_tests << std::endl;

    if (report.failed_tests == 0) {
        std::cout << "\n>>> EMPIRICAL VERDICT: ALL TESTS PASSED [APPROVE] <<<" << std::endl;
        return 0;
    } else {
        std::cout << "\n>>> EMPIRICAL VERDICT: FAILURES DETECTED [REJECT] <<<" << std::endl;
        for (const auto& f : report.failures) {
            std::cout << "  - " << f << std::endl;
        }
        return 1;
    }
}
