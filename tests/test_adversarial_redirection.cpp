#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kernel/disc.h"
#include "kernel/kernel.h"

namespace fs = std::filesystem;

// Minimal kernel stubs required for standalone test linking with files.cpp.o and disc.cpp.o
namespace kernel {
uint8_t* g_base = nullptr;
void RegisterExport(uint32_t, const char*, KernelFn) {}
ObjectRef FromHandle(uint32_t) { return nullptr; }
int32_t ResetEvent(Object&) { return 0; }
int32_t SetEvent(Object&) { return 0; }
uint32_t NewHandle(std::shared_ptr<Object>) { return 1; }
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

int main(int argc, char** argv) {
    // CLI Probe Mode for automated E2E test runner integration:
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--resolve" && i + 1 < argc) {
            kernel::MountDefaults("assets", "assets/hdd");
            auto disc = std::make_unique<kernel::Disc>();
            std::string err;
            disc->Open("assets/Black.iso", err);
            kernel::MountDisc(std::move(disc));
            std::string p = kernel::ResolvePath(argv[++i], 0);
            std::cout << p << std::endl;
            return p.empty() ? 1 : 0;
        }
        if (arg == "--read-size" && i + 1 < argc) {
            kernel::MountDefaults("assets", "assets/hdd");
            auto disc = std::make_unique<kernel::Disc>();
            std::string err;
            disc->Open("assets/Black.iso", err);
            kernel::MountDisc(std::move(disc));
            std::vector<uint8_t> data;
            try {
                if (kernel::ReadGameFile(argv[++i], data)) {
                    std::cout << "SIZE " << data.size() << std::endl;
                    return 0;
                }
            } catch (const std::exception& e) {
                std::cerr << "EXCEPTION: " << e.what() << std::endl;
                return 2;
            }
            std::cout << "FAIL" << std::endl;
            return 1;
        }
    }

    std::cout << "============================================================" << std::endl;
    std::cout << "EMPIRICAL ADVERSARIAL STRESS HARNESS: files.cpp Redirection" << std::endl;
    std::cout << "============================================================" << std::endl;

    TestReport report;

    // Step 0: Initialize Kernel Mounts
    std::cout << "\n--- Step 0: Initializing Kernel Environment ---" << std::endl;
    kernel::MountDefaults("assets", "assets/hdd");
    auto disc = std::make_unique<kernel::Disc>();
    std::string err;
    bool iso_ok = disc->Open("assets/Black.iso", err);
    report.Assert(iso_ok, "assets/Black.iso opened successfully: " + err);
    if (!iso_ok) {
        std::cerr << "CRITICAL: Cannot open assets/Black.iso. Aborting tests." << std::endl;
        return 1;
    }
    kernel::MountDisc(std::move(disc));
    report.Assert(kernel::MountedDisc() != nullptr, "MountedDisc() is non-null");

    // =========================================================================
    // SUITE 1: Path Normalization, Slashes, and DOS Prefixes
    // =========================================================================
    std::cout << "\n--- Suite 1: Path Slashes, Normalization & Prefixes ---" << std::endl;
    {
        // 1.1 Standard backslash
        std::string p1 = kernel::ResolvePath("D:\\Levels\\Level_00\\LevelDat.bin", 0);
        report.Assert(!p1.empty() && p1.find("level_mp00/LevelDat.bin") != std::string::npos,
                      "1.1 Standard backslash: D:\\Levels\\Level_00\\LevelDat.bin -> " + p1);

        // 1.2 Standard forward slash
        std::string p2 = kernel::ResolvePath("D:/Levels/Level_00/LevelDat.bin", 0);
        report.Assert(!p2.empty() && p2.find("level_mp00/LevelDat.bin") != std::string::npos,
                      "1.2 Standard forward slash: D:/Levels/Level_00/LevelDat.bin -> " + p2);

        // 1.3 Lowercase drive letter with forward slash
        std::string p3 = kernel::ResolvePath("d:/levels/level_00/leveldat.bin", 0);
        report.Assert(!p3.empty() && p3.find("level_mp00/LevelDat.bin") != std::string::npos,
                      "1.3 Lowercase drive + forward slash: d:/levels/level_00/leveldat.bin -> " + p3);

        // 1.4 Alternating slashes
        std::string p4 = kernel::ResolvePath("D:\\levels/level_00\\stg_0001/guns.bin", 0);
        report.Assert(!p4.empty() && p4.find("stg_0001/guns.bin") != std::string::npos,
                      "1.4 Alternating slashes: D:\\levels/level_00\\stg_0001/guns.bin -> " + p4);

        // 1.5 Multiple consecutive slashes
        std::string p5 = kernel::ResolvePath("D://///Levels////Level_00///LevelDat.bin", 0);
        report.Assert(!p5.empty() && p5.find("level_mp00/LevelDat.bin") != std::string::npos,
                      "1.5 Consecutive slashes: D://///Levels////Level_00///LevelDat.bin -> " + p5);

        // 1.6 Explicit NT DOS device path
        std::string p6 = kernel::ResolvePath("\\??\\D:\\Levels\\Level_00\\LevelDat.bin", 0);
        report.Assert(!p6.empty() && p6.find("level_mp00/LevelDat.bin") != std::string::npos,
                      "1.6 NT DOS device path: \\??\\D:\\Levels\\Level_00\\LevelDat.bin -> " + p6);

        // 1.7 Forward slash NT DOS device path
        std::string p7 = kernel::ResolvePath("/\?\?/D:/Levels/Level_00/LevelDat.bin", 0);
        report.Assert(!p7.empty() && p7.find("level_mp00/LevelDat.bin") != std::string::npos,
                      "1.7 Unix-style NT path: /\\?\\?/D:/Levels/Level_00/LevelDat.bin -> " + p7);

        // 1.8 Dot components in path
        std::string p8 = kernel::ResolvePath("D:\\Levels\\.\\Level_00\\.\\LevelDat.bin", 0);
        report.Assert(!p8.empty() && p8.find("level_mp00/LevelDat.bin") != std::string::npos,
                      "1.8 Dot components: D:\\Levels\\.\\Level_00\\.\\LevelDat.bin -> " + p8);

        // 1.9 Dot-dot components in path
        std::string p9 = kernel::ResolvePath("D:\\Levels\\Level_01\\..\\Level_00\\LevelDat.bin", 0);
        report.Assert(!p9.empty() && p9.find("level_mp00/LevelDat.bin") != std::string::npos,
                      "1.9 Dot-dot components: D:\\Levels\\Level_01\\..\\Level_00\\LevelDat.bin -> " + p9);
    }

    // =========================================================================
    // SUITE 2: Linux ext4 Case Permutations
    // =========================================================================
    std::cout << "\n--- Suite 2: Linux ext4 Case Permutations ---" << std::endl;
    {
        // 2.1 All UPPERCASE
        std::string c1 = kernel::ResolvePath("D:\\LEVELS\\LEVEL_00\\LEVELDAT.BIN", 0);
        report.Assert(!c1.empty() && c1.find("LevelDat.bin") != std::string::npos,
                      "2.1 All UPPERCASE: D:\\LEVELS\\LEVEL_00\\LEVELDAT.BIN -> " + c1);

        // 2.2 All lowercase
        std::string c2 = kernel::ResolvePath("D:\\levels\\level_00\\leveldat.bin", 0);
        report.Assert(!c2.empty() && c2.find("LevelDat.bin") != std::string::npos,
                      "2.2 All lowercase: D:\\levels\\level_00\\leveldat.bin -> " + c2);

        // 2.3 Mixed CamelCase
        std::string c3 = kernel::ResolvePath("D:\\LeVeLs\\LeVeL_00\\LevelDat.bin", 0);
        report.Assert(!c3.empty() && c3.find("LevelDat.bin") != std::string::npos,
                      "2.3 Mixed CamelCase: D:\\LeVeLs\\LeVeL_00\\LevelDat.bin -> " + c3);

        // 2.4 Inverted case
        std::string c4 = kernel::ResolvePath("D:\\lEVELS\\lEVEL_00\\lEVELdAT.bIN", 0);
        report.Assert(!c4.empty() && c4.find("LevelDat.bin") != std::string::npos,
                      "2.4 Inverted case: D:\\lEVELS\\lEVEL_00\\lEVELdAT.bIN -> " + c4);

        // 2.5 Subdirectory casing: stg_0001
        std::string c5_a = kernel::ResolvePath("D:\\LEVELS\\LEVEL_00\\STG_0001\\STLEVEL.BIN", 0);
        std::string c5_b = kernel::ResolvePath("D:\\levels\\level_00\\stg_0001\\stlevel.bin", 0);
        std::string c5_c = kernel::ResolvePath("D:\\LeVeLs\\LeVeL_00\\StG_0001\\sTlEvEl.BiN", 0);
        report.Assert(!c5_a.empty() && c5_a.find("StLevel.bin") != std::string::npos,
                      "2.5a Subdirectory UPPER: D:\\LEVELS\\LEVEL_00\\STG_0001\\STLEVEL.BIN -> " + c5_a);
        report.Assert(!c5_b.empty() && c5_b.find("StLevel.bin") != std::string::npos,
                      "2.5b Subdirectory lower: D:\\levels\\level_00\\stg_0001\\stlevel.bin -> " + c5_b);
        report.Assert(!c5_c.empty() && c5_c.find("StLevel.bin") != std::string::npos,
                      "2.5c Subdirectory mixed: D:\\LeVeLs\\LeVeL_00\\StG_0001\\sTlEvEl.BiN -> " + c5_c);

        // 2.6 Nested weapon database (fpguns)
        std::string c6_a = kernel::ResolvePath("D:\\LEVELS\\LEVEL_00\\FPGUNS\\BG1_AK1.DB", 0);
        std::string c6_b = kernel::ResolvePath("D:\\levels\\level_00\\fpguns\\bg1_ak1.db", 0);
        std::string c6_c = kernel::ResolvePath("D:\\Levels\\Level_00\\FpGuns\\Bg1_Ak1.Db", 0);
        report.Assert(!c6_a.empty() && c6_a.find("bg1_ak1.db") != std::string::npos,
                      "2.6a Nested fpguns UPPER: " + c6_a);
        report.Assert(!c6_b.empty() && c6_b.find("bg1_ak1.db") != std::string::npos,
                      "2.6b Nested fpguns lower: " + c6_b);
        report.Assert(!c6_c.empty() && c6_c.find("bg1_ak1.db") != std::string::npos,
                      "2.6c Nested fpguns mixed: " + c6_c);

        // 2.7 Direct Level_MP00 requests
        std::string c7_a = kernel::ResolvePath("D:\\LEVELS\\LEVEL_MP00\\LEVELDAT.BIN", 0);
        std::string c7_b = kernel::ResolvePath("D:\\levels\\level_mp00\\leveldat.bin", 0);
        std::string c7_c = kernel::ResolvePath("D:\\LeVeLs\\LeVeL_mP00\\LevelDat.bin", 0);
        report.Assert(!c7_a.empty() && c7_a.find("LevelDat.bin") != std::string::npos,
                      "2.7a Level_MP00 UPPER: " + c7_a);
        report.Assert(!c7_b.empty() && c7_b.find("LevelDat.bin") != std::string::npos,
                      "2.7b Level_MP00 lower: " + c7_b);
        report.Assert(!c7_c.empty() && c7_c.find("LevelDat.bin") != std::string::npos,
                      "2.7c Level_MP00 mixed: " + c7_c);

        // 2.8 Exhaustive check of all 45 extracted files in assets/levels/level_mp00
        std::cout << "  ... Checking exhaustive resolution of all extracted files ..." << std::endl;
        int files_tested = 0;
        int files_resolved_upper = 0;
        int files_resolved_lower = 0;
        for (const auto& entry : fs::recursive_directory_iterator("assets/levels/level_mp00")) {
            if (entry.is_regular_file()) {
                files_tested++;
                std::string rel = entry.path().lexically_relative("assets/levels/level_mp00").string();
                std::replace(rel.begin(), rel.end(), '/', '\\');

                // UPPERCASE test
                std::string rel_upper = rel;
                for (char& ch : rel_upper) ch = char(std::toupper(uint8_t(ch)));
                std::string p_up = kernel::ResolvePath("D:\\Levels\\Level_00\\" + rel_upper, 0);
                if (!p_up.empty() && fs::exists(p_up)) files_resolved_upper++;

                // LOWERCASE test
                std::string rel_lower = rel;
                for (char& ch : rel_lower) ch = char(std::tolower(uint8_t(ch)));
                std::string p_lo = kernel::ResolvePath("D:\\Levels\\Level_00\\" + rel_lower, 0);
                if (!p_lo.empty() && fs::exists(p_lo)) files_resolved_lower++;
            }
        }
        report.Assert(files_tested == 45, "2.8a Total files in level_mp00 is exactly 45 (found: " + std::to_string(files_tested) + ")");
        report.Assert(files_resolved_upper == 45, "2.8b All 45 files resolved via UPPERCASE request (passed: " + std::to_string(files_resolved_upper) + "/45)");
        report.Assert(files_resolved_lower == 45, "2.8c All 45 files resolved via LOWERCASE request (passed: " + std::to_string(files_resolved_lower) + "/45)");
    }

    // =========================================================================
    // SUITE 3: Host File Priority (Redirection Precedence over ISO)
    // =========================================================================
    std::cout << "\n--- Suite 3: Host File Priority over ISO ---" << std::endl;
    {
        // 3.1 LevelDat.bin exists in both host and ISO
        std::string p = kernel::ResolvePath("D:\\Levels\\Level_00\\LevelDat.bin", 0);
        report.Assert(!p.empty(), "3.1a LevelDat.bin resolves to non-empty host path: " + p);
        report.Assert(p.find("level_mp00/LevelDat.bin") != std::string::npos, "3.1b LevelDat.bin redirected to level_mp00");

        std::vector<uint8_t> data;
        bool ok = kernel::ReadGameFile("D:\\Levels\\Level_00\\LevelDat.bin", data);
        report.Assert(ok && data.size() == 711088, "3.1c ReadGameFile(LevelDat.bin) returned 711,088 bytes");
        uint32_t magic = 0;
        if (data.size() >= 4) std::memcpy(&magic, data.data(), 4);
        report.Assert(magic == 0x11, "3.1d LevelDat.bin magic is 0x11 (world spatial data)");

        // 3.2 CRITICAL EMPIRICAL PROOF: speech.slb
        // Retail ISO has 6,944 bytes. Host clean map has 104 bytes dummy with 0 cues.
        std::vector<uint8_t> spch_data;
        bool spch_ok = kernel::ReadGameFile("D:\\Levels\\Level_00\\speech.slb", spch_data);
        report.Assert(spch_ok, "3.2a ReadGameFile(speech.slb) succeeded");
        report.Assert(spch_data.size() == 104,
                      "3.2b EMPIRICAL PROOF: speech.slb loaded from HOST (104 bytes), NOT retail ISO (6,944 bytes)! Actual: " +
                      std::to_string(spch_data.size()) + " bytes");
        if (spch_data.size() == 104) {
            uint32_t slb_magic = 0, num_cues = 0;
            std::memcpy(&slb_magic, spch_data.data() + 4, 4);
            std::memcpy(&num_cues, spch_data.data() + 0x54, 4);
            report.Assert(slb_magic == 0x474E494B, "3.2c speech.slb magic is 'KING' (0x474E494B)");
            report.Assert(num_cues == 0, "3.2d speech.slb contains exactly 0 active dialogue cues (suppressed)");
        }

        // 3.3 Unit_01.bin (10.5 MB geometry chunk)
        std::vector<uint8_t> unit_data;
        bool unit_ok = kernel::ReadGameFile("D:\\Levels\\Level_00\\Unit_01.bin", unit_data);
        report.Assert(unit_ok && unit_data.size() == 10526464, "3.3 Unit_01.bin loaded from host (10,526,464 bytes)");
    }

    // =========================================================================
    // SUITE 4: Graceful ISO Fallback
    // =========================================================================
    std::cout << "\n--- Suite 4: Graceful ISO Fallback ---" << std::endl;
    {
        // 4.1 spch_EN.bks was stripped from host clean map, exists in retail ISO (8,814,592 bytes)
        std::string fallback_host = kernel::ResolvePath("D:\\Levels\\Level_00\\spch_EN.bks", 0);
        report.Assert(fallback_host.empty(), "4.1a ResolvePath on non-extracted file returns empty host path (confirming ISO target)");

        std::vector<uint8_t> bks_data;
        bool bks_ok = kernel::ReadGameFile("D:\\Levels\\Level_00\\spch_EN.bks", bks_data);
        report.Assert(bks_ok, "4.1b ReadGameFile for non-extracted spch_EN.bks succeeded via ISO fallback");
        report.Assert(bks_data.size() == 8814592,
                      "4.1c spch_EN.bks size matches retail ISO exactly: 8,814,592 bytes (actual: " +
                      std::to_string(bks_data.size()) + ")");

        // 4.2 Fallback with Level_MP00 request mapping to Level_00 on ISO
        std::vector<uint8_t> mp_bks_data;
        bool mp_bks_ok = kernel::ReadGameFile("D:\\Levels\\Level_MP00\\spch_EN.bks", mp_bks_data);
        report.Assert(mp_bks_ok && mp_bks_data.size() == 8814592,
                      "4.2 Requesting D:\\Levels\\Level_MP00\\spch_EN.bks falls back to Level_00 on ISO (8,814,592 bytes)");

        // 4.3 ISO Fallback with mixed casing
        std::vector<uint8_t> bks_upper;
        bool bks_up_ok = kernel::ReadGameFile("D:\\LEVELS\\LEVEL_00\\SPCH_EN.BKS", bks_upper);
        report.Assert(bks_up_ok && bks_upper.size() == 8814592, "4.3a ISO Fallback with UPPERCASE: D:\\LEVELS\\LEVEL_00\\SPCH_EN.BKS");

        std::vector<uint8_t> bks_lower;
        bool bks_lo_ok = kernel::ReadGameFile("D:\\levels\\level_00\\spch_en.bks", bks_lower);
        report.Assert(bks_lo_ok && bks_lower.size() == 8814592, "4.3b ISO Fallback with lowercase: D:\\levels\\level_00\\spch_en.bks");

        // 4.4 Campaign cutscene in root videos/ folder on ISO
        std::vector<uint8_t> xmv_data;
        bool xmv_ok = kernel::ReadGameFile("D:\\videos\\02_n.xmv", xmv_data);
        report.Assert(xmv_ok && xmv_data.size() == 21221376,
                      "4.4 ReadGameFile(videos/02_n.xmv) loaded from ISO (21,221,376 bytes)");
    }

    // =========================================================================
    // SUITE 5: Non-Existent Files & Hostile Boundary Probes
    // =========================================================================
    std::cout << "\n--- Suite 5: Non-Existent Files & Hostile Probes ---" << std::endl;
    {
        // 5.1 Non-existent file inside valid level folder
        std::vector<uint8_t> bad_data;
        bool bad_1 = kernel::ReadGameFile("D:\\Levels\\Level_00\\TotallyFakeFile_12345.bin", bad_data);
        report.Assert(!bad_1, "5.1 Non-existent file in level returns false without crash");

        // 5.2 Non-existent level
        bool bad_2 = kernel::ReadGameFile("D:\\Levels\\Level_99\\LevelDat.bin", bad_data);
        report.Assert(!bad_2, "5.2 Non-existent level (Level_99) returns false without crash");

        // 5.3 Completely non-existent top-level path
        bool bad_3 = kernel::ReadGameFile("D:\\GibberishDirectory\\ImaginaryFile.dat", bad_data);
        report.Assert(!bad_3, "5.3 Non-existent directory returns false without crash");

        // 5.4 Directory traversal attempt
        bool bad_4 = kernel::ReadGameFile("D:\\Levels\\Level_00\\..\\..\\..\\..\\etc\\passwd", bad_data);
        report.Assert(!bad_4, "5.4 Directory traversal attack (../../../../etc/passwd) safely rejected");

        // 5.5 Illegal characters (colons) in Xbox path
        bool bad_5 = kernel::ReadGameFile("D:\\Invalid:Stream\\Name.bin", bad_data);
        report.Assert(!bad_5, "5.5 Path with illegal colon rejected");

        // 5.6 Empty and root queries
        bool bad_6 = kernel::ReadGameFile("", bad_data);
        report.Assert(!bad_6, "5.6 Empty path string safely returns false");

        std::string root_p = kernel::ResolvePath("D:", 0);
        report.Assert(!root_p.empty(), "5.7 'D:' resolves safely without crash");

        std::string root_slash = kernel::ResolvePath("D:\\", 0);
        report.Assert(!root_slash.empty(), "5.8 'D:\\' resolves safely without crash");

        // 5.9 Buffer overflow probe (long path)
        std::string long_path = "D:\\Levels\\Level_00\\";
        for (int i = 0; i < 200; i++) long_path += "subdir_segment_/";
        long_path += "file.bin";
        bool bad_long = kernel::ReadGameFile(long_path, bad_data);
        report.Assert(!bad_long, "5.9 Deeply nested long path (> 3,000 chars) handled safely without overflow");

        // 5.10 Directory read attempt (must return false without crashing or throwing std::__ios_failure)
        bool dir_read_threw = false;
        bool dir_read_ret = false;
        try {
            dir_read_ret = kernel::ReadGameFile("D:\\Levels\\Level_00", bad_data);
        } catch (const std::exception& e) {
            dir_read_threw = true;
        }
        report.Assert(!dir_read_threw && !dir_read_ret,
                      "5.10 ReadGameFile on directory safely returns false without std::__ios_failure exception");
    }

    // =========================================================================
    // SUITE 6: Concurrency & Stress Harness
    // =========================================================================
    std::cout << "\n--- Suite 6: Concurrency & Multi-Threaded Stress ---" << std::endl;
    {
        constexpr int kNumThreads = 8;
        constexpr int kOpsPerThread = 250;
        std::vector<std::thread> workers;
        std::atomic<int> success_count{0};
        std::atomic<int> error_count{0};

        std::cout << "  ... Launching " << kNumThreads << " threads x " << kOpsPerThread
                  << " ops (" << (kNumThreads * kOpsPerThread) << " total concurrent operations) ..." << std::endl;

        auto start_time = std::chrono::steady_clock::now();

        for (int tid = 0; tid < kNumThreads; tid++) {
            workers.emplace_back([tid, &success_count, &error_count]() {
                for (int i = 0; i < kOpsPerThread; i++) {
                    if (tid % 2 == 0) {
                        // Host file read: LevelDat.bin with alternating casing
                        std::string path = (i % 2 == 0)
                                               ? "D:\\Levels\\Level_00\\LevelDat.bin"
                                               : "d:/levels/level_00/leveldat.bin";
                        std::vector<uint8_t> d;
                        if (kernel::ReadGameFile(path, d) && d.size() == 711088) {
                            success_count++;
                        } else {
                            error_count++;
                        }
                    } else {
                        // ISO fallback read: spch_EN.bks or speech.slb
                        if (i % 2 == 0) {
                            std::vector<uint8_t> d;
                            if (kernel::ReadGameFile("D:\\Levels\\Level_00\\speech.slb", d) && d.size() == 104) {
                                success_count++;
                            } else {
                                error_count++;
                            }
                        } else {
                            // Non-existent probe stress
                            std::vector<uint8_t> d;
                            if (!kernel::ReadGameFile("D:\\Levels\\Level_00\\nonexistent.bin", d)) {
                                success_count++;
                            } else {
                                error_count++;
                            }
                        }
                    }
                }
            });
        }

        for (auto& w : workers) w.join();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time);

        report.Assert(error_count == 0,
                      "6.1 Multi-threaded concurrent file operations: 0 errors (errors: " + std::to_string(error_count.load()) + ")");
        report.Assert(success_count == kNumThreads * kOpsPerThread,
                      "6.2 Concurrent operations completed: " + std::to_string(success_count.load()) +
                      "/" + std::to_string(kNumThreads * kOpsPerThread) + " in " + std::to_string(elapsed.count()) + " ms");
    }

    // =========================================================================
    // FINAL REPORT & SUMMARY
    // =========================================================================
    std::cout << "\n============================================================" << std::endl;
    std::cout << "TEST EXECUTION SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "Total Tests Executed: " << report.total_tests << std::endl;
    std::cout << "Passed: " << report.passed_tests << std::endl;
    std::cout << "Failed: " << report.failed_tests << std::endl;

    if (report.failed_tests == 0) {
        std::cout << "\n>>> VERDICT: ALL TESTS PASSED [APPROVE] <<<" << std::endl;
        return 0;
    } else {
        std::cout << "\n>>> VERDICT: FAILURES DETECTED [REJECT] <<<" << std::endl;
        for (const auto& f : report.failures) {
            std::cout << "  - " << f << std::endl;
        }
        return 1;
    }
}
