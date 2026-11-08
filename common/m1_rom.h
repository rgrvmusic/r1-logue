// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once
/**
 * The M1 ROM set, read at unit_init into the tables the engine takes.
 *
 * Files in <userfs>/Programs, all required:
 *
 *   m1-ic30.bin        64 KB coefficient ROM (IC20/IC26 are the same chip;
 *                      any of the three is accepted)
 *   m1-ic36.bin ..      eight 512 KB PCM halves, IC36-39 low bytes and
 *   m1-ic43.bin         IC40-43 high bytes
 *   m1-ic23.bin,        the two 64 KB halves of the original M1 OS (Rev 19);
 *   m1-ic32.bin         which one is the even half is found by content
 *
 * Optional:
 *
 *   m1-preload.syx      an M1 ALL DATA dump; without it the unit builds a
 *                      plain program per multisound
 *
 * The Plus+1 OS is rejected, as is anything that fails the checks in
 * doc/ENGINE.md of PicoFaceM1.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "m1_engine/m1_engine.h"

namespace m1unit {

class M1Rom {
public:
    /* Reads the set and fills tables(). False with failure() set otherwise. */
    bool load(const char* dir, const char* preloadName) {
        dir_ = dir;
        if (!loadPcm()) return false;
        if (!loadCoef()) return false;
        if (!loadOs()) return false;
        if (!buildTables()) return false;
        loadPreload(preloadName);
        return true;
    }

    const m1::RomTables& tables() const { return tables_; }
    int16_t* fxMem() { return fxMem_.data(); }
    bool ready() const { return ready_; }
    bool hasPrograms() const { return hasPrograms_; }
    const char* failure() const { return failure_; }

    const uint8_t* program(int index) const {
        if (!hasPrograms_ || index < 0 || index >= m1::kPrograms) return nullptr;
        return programs_.data() + (size_t)m1::kProgLen * (size_t)index;
    }
    const char* multisoundName(int index) const {
        if (index < 0 || index >= 100 || ms_.empty()) return "---";
        static char name[12];
        std::snprintf(name, sizeof name, "%s", ms_[(size_t)index].name);
        return name;
    }

private:
    /* DS is at F75C in the CPU map; the 128 KB image starts at E0000, so the
     * DS offset in the image is F75C0 - E0000. */
    static constexpr size_t kDsBase = 0x175C0;
    static constexpr size_t kOsSize = 0x20000;
    static constexpr size_t kPcmWords = 0x200000;
    static constexpr size_t kCoefSize = 0x10000;

    static constexpr size_t kEgRise = 0x10C22;
    static constexpr size_t kEgFall = 0x10E22;
    static constexpr size_t kVelCurve = 0x116AB;
    static constexpr size_t kMgDelay = 0x10B5A;
    static constexpr size_t kMgIntP = 0x109CA;
    static constexpr size_t kMgIntF = 0x10A92;
    static constexpr size_t kMgFreq = kDsBase + 0x738E;
    static constexpr size_t kPitchS = kDsBase + 0x0616;
    static constexpr size_t kPitchF = kDsBase + 0x0748;
    static constexpr size_t kPanReg = kDsBase + 0x0854;
    static constexpr size_t kScale = kDsBase + 0x0848;
    static constexpr size_t kPan101 = kDsBase + 0x8272;
    static constexpr size_t kDelayStart = kDsBase + 0x7230;
    static constexpr size_t kMsTable = kDsBase + 0x6978;
    static constexpr size_t kDrums = kDsBase + 0x5B9E;

    static uint8_t u8(const uint8_t* p) { return p[0]; }
    static int8_t s8(const uint8_t* p) {
        return static_cast<int8_t>(p[0]);
    }
    static uint16_t u16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    static int16_t s16(const uint8_t* p) {
        return static_cast<int16_t>(p[0] | (p[1] << 8));
    }

    bool fail(const char* why) {
        std::snprintf(failure_, sizeof failure_, "%s", why);
        return false;
    }

    bool join(char* out, size_t n, const char* name) const {
        const int written = std::snprintf(out, n, "%s/%s", dir_, name);
        return written > 0 && (size_t)written < n;
    }

    bool readFile(const char* path, std::vector<uint8_t>& out, size_t expected) {
        FILE* file = std::fopen(path, "rb");
        if (!file) return false;
        out.resize(expected);
        const size_t got = std::fread(out.data(), 1, expected, file);
        const bool extra = std::fgetc(file) != EOF;
        std::fclose(file);
        if (got != expected || extra) {
            out.clear();
            return false;
        }
        return true;
    }

    bool readWhole(const char* path, std::vector<uint8_t>& out) {
        FILE* file = std::fopen(path, "rb");
        if (!file) return false;
        if (std::fseek(file, 0, SEEK_END) != 0) {
            std::fclose(file);
            return false;
        }
        const long size = std::ftell(file);
        if (size <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
            std::fclose(file);
            return false;
        }
        out.resize((size_t)size);
        const size_t got = std::fread(out.data(), 1, out.size(), file);
        std::fclose(file);
        return got == out.size();
    }

    /* A PCM half: eight 512 KB files, IC36-39 low and IC40-43 high. */
    bool loadPcm() {
        pcm_.assign(kPcmWords, 0);
        uint8_t* dst = reinterpret_cast<uint8_t*>(pcm_.data());
        for (int k = 0; k < 4; ++k) {
            char loName[32], hiName[32], path[512];
            std::snprintf(loName, sizeof loName, "m1-ic%d.bin", 36 + k);
            std::snprintf(hiName, sizeof hiName, "m1-ic%d.bin", 40 + k);
            if (!join(path, sizeof path, loName) || !readFile(path, scratch_, 0x80000))
                return fail("PCM low half missing");
            const size_t base = (size_t)k * 0x100000;
            for (size_t i = 0; i < 0x80000; ++i) dst[base + 2 * i] = scratch_[i];
            if (!join(path, sizeof path, hiName) || !readFile(path, scratch_, 0x80000))
                return fail("PCM high half missing");
            for (size_t i = 0; i < 0x80000; ++i) dst[base + 2 * i + 1] = scratch_[i];
        }
        scratch_.clear();
        return true;
    }

    /* The coefficient ROM: the interpolation kernel in the first 32 KB and
     * the VDF coefficient pairs at 0x8000 and 0xC000. */
    bool loadCoef() {
        static const char* const names[] = {"m1-ic30.bin", "m1-ic20.bin", "m1-ic26.bin"};
        bool found = false;
        for (const char* name : names) {
            char path[512];
            if (!join(path, sizeof path, name)) continue;
            if (readFile(path, coef_, kCoefSize)) {
                found = true;
                break;
            }
        }
        if (!found) return fail("coefficient ROM missing");
        /* All 4096 phases of the kernel have the same tap sum. */
        for (int phase = 0; phase < 4096; phase += 97) {
            const uint8_t* r = &coef_[(size_t)phase * 8];
            const int sum = (s8(r) + s8(r + 1)) + s8(r + 2) * 2 + (r[3] + r[4]) * 8 +
                            s8(r + 5) * 2 + (s8(r + 6) + s8(r + 7));
            if (sum != 1226) return fail("not the M1 coefficient ROM");
        }
        return true;
    }

    /* The two OS halves. The even half is the one whose interleaving puts the
     * multisound table where Rev 19 has it. */
    bool loadOs() {
        std::vector<uint8_t> a, b;
        char path[512];
        if (!join(path, sizeof path, "m1-ic23.bin") || !readFile(path, a, 0x10000))
            return fail("OS half m1-ic23.bin missing");
        if (!join(path, sizeof path, "m1-ic32.bin") || !readFile(path, b, 0x10000))
            return fail("OS half m1-ic32.bin missing");
        if (interleave(a, b) || interleave(b, a)) return true;
        return fail("not the original M1 Rev 19 OS");
    }

    bool interleave(const std::vector<uint8_t>& even, const std::vector<uint8_t>& odd) {
        os_.resize(kOsSize);
        for (size_t i = 0; i < 0x10000; ++i) {
            os_[2 * i] = even[i];
            os_[2 * i + 1] = odd[i];
        }
        /* Multisound 0 is "Piano", 1 is "E.Piano 1" (14-byte records). */
        return std::memcmp(&os_[kMsTable + 4], "Piano     ", 10) == 0 &&
               std::memcmp(&os_[kMsTable + 14 + 4], "E.Piano 1 ", 10) == 0;
    }

    template <typename T>
    static void readTable(const uint8_t* base, size_t count, std::vector<T>& out) {
        out.resize(count);
        for (size_t i = 0; i < count; ++i)
            out[i] = (sizeof(T) == 1) ? static_cast<T>(base[i]) : u16(base + 2 * i);
    }

    /* A DS-relative pointer, or nullptr when the offset leaves the image. */
    const uint8_t* ds(uint16_t off) const {
        const size_t at = kDsBase + off;
        return at + 16 <= os_.size() ? &os_[at] : nullptr;
    }
    const uint8_t* lin(size_t off) const { return &os_[off]; }

    bool buildTables() {
        /* Sanity checks: each table has to look like the one it is. */
        if (u16(lin(kVelCurve) + 2 * 252) != 15629) return fail("velocity table misplaced");
        if (u16(lin(kPitchS) + 2 * 128) != 4096) return fail("pitch table misplaced");
        if (u16(lin(kMgIntP) + 2 * 99) != 8191) return fail("MG depth table misplaced");
        if (lin(kScale)[0] != 0 || s8(lin(kScale) + 1) != -38 || s8(lin(kScale) + 2) != 5)
            return fail("scale table misplaced");
        if (u16(lin(kEgRise)) == 0) return fail("EG rise table misplaced");

        readTable<uint16_t>(lin(kEgRise), 256, egRise_);
        readTable<uint16_t>(lin(kEgFall), 256, egFall_);
        readTable<uint16_t>(lin(kVelCurve), 256, velCurve_);
        readTable<uint16_t>(lin(kMgFreq), 100, mgFreq_);
        readTable<uint16_t>(lin(kMgDelay), 100, mgDelay_);
        readTable<uint16_t>(lin(kMgIntP), 100, mgIntP_);
        readTable<uint16_t>(lin(kMgIntF), 100, mgIntF_);
        readTable<uint16_t>(lin(kPitchS), 153, pitchS_);
        readTable<uint16_t>(lin(kPitchF), 128, pitchF_);
        readTable<uint16_t>(lin(kPanReg), 14, panReg_);
        readTable<uint16_t>(lin(kPan101), 101, pan101_);
        readTable<uint16_t>(lin(kDelayStart), 100, delayStart_);

        scale_.resize(12);
        for (size_t i = 0; i < 12; ++i) scale_[i] = s8(lin(kScale) + i);

        /* VDF coefficient tables: each 16 KB area holds T0 then the k table,
         * 4096 words each, split low/high over 4 KB. */
        auto words = [this](size_t base) {
            std::vector<uint16_t> v(4096);
            for (size_t i = 0; i < 4096; ++i)
                v[i] = static_cast<uint16_t>(coef_[base + i] | (coef_[base + 0x1000 + i] << 8));
            return v;
        };
        auto signedWords = [](const std::vector<uint16_t>& v) {
            std::vector<int16_t> s(v.size());
            for (size_t i = 0; i < v.size(); ++i) s[i] = static_cast<int16_t>(v[i]);
            return s;
        };
        vdf1k_ = words(0x8000 + 0x2000);
        vdf2k_ = words(0xC000 + 0x2000);
        vdf1t0_ = signedWords(words(0x8000));
        vdf2t0_ = signedWords(words(0xC000));

        if (!readMultisounds()) return false;
        if (!readDrums()) return false;

        programs_.assign((size_t)m1::kProgLen * m1::kPrograms, 0);
        combis_.assign((size_t)m1::kCombiLen * m1::kCombis, 0);
        kits_.assign((size_t)m1::kKitLen * 4, 0);
        global_.assign(21, 0);   /* equal temperament until a preload says otherwise */

        fxMem_.assign(65536, 0);

        tables_.pcm = pcm_.data();
        tables_.pcmWords = (uint32_t)pcm_.size();
        tables_.ms = ms_.data();
        tables_.splits = splits_.data();
        tables_.drums = drums_.data();
        tables_.nDrums = 44;
        tables_.kernel = coef_.data();
        tables_.vdf1k = vdf1k_.data();
        tables_.vdf2k = vdf2k_.data();
        tables_.vdf1t0 = vdf1t0_.data();
        tables_.vdf2t0 = vdf2t0_.data();
        tables_.egRise = egRise_.data();
        tables_.egFall = egFall_.data();
        tables_.velCurve = velCurve_.data();
        tables_.mgFreq = mgFreq_.data();
        tables_.mgDelay = mgDelay_.data();
        tables_.mgIntP = mgIntP_.data();
        tables_.mgIntF = mgIntF_.data();
        tables_.pitchS = pitchS_.data();
        tables_.pitchF = pitchF_.data();
        tables_.panReg = panReg_.data();
        tables_.pan101 = pan101_.data();
        tables_.delayStart = delayStart_.data();
        tables_.fx = &fx_;   /* zero: the effect section is bypassed */
        tables_.programs = programs_.data();
        tables_.combis = combis_.data();
        tables_.kits = kits_.data();
        tables_.global = global_.data();
        tables_.scale = scale_.data();
        ready_ = true;
        return true;
    }

    bool readMultisounds() {
        ms_.resize(100);
        splits_.clear();
        splits_.reserve(512);
        for (int i = 0; i < 100; ++i) {
            const uint8_t* rec = ds(static_cast<uint16_t>(0x6978 + 14 * i));
            const uint16_t list = u16(rec);
            const uint8_t kind = rec[2];
            const uint8_t count = rec[3];
            m1::Multisound& ms = ms_[(size_t)i];
            ms.first = (uint16_t)splits_.size();
            ms.count = count;
            ms.dwgs = kind == 2 ? 1 : 0;
            std::memset(ms.name, 0, sizeof ms.name);
            std::memcpy(ms.name, rec + 4, 10);
            ms.name[10] = 0;
            const int waveOffset = ms.dwgs ? (i - 77) * 0x3FC : 0;
            for (int k = 0; k < count; ++k) {
                /* 10-byte split records; the first 9 bytes are the fields. */
                const uint8_t* sp = ds(static_cast<uint16_t>(list + 10 * k));
                const uint8_t* r = sp ? ds(u16(sp)) : nullptr;
                if (!sp || !r) return fail("multisound record outside the OS image");
                m1::Split s;
                std::memset(&s, 0, sizeof s);
                s.tune128 = s16(r);
                const uint32_t start = (uint32_t)r[2] | ((uint32_t)r[3] << 8) | ((uint32_t)r[4] << 16);
                const uint32_t loop = (uint32_t)r[6] | ((uint32_t)r[7] << 8) | ((uint32_t)r[8] << 16);
                const uint32_t end = (uint32_t)r[10] | ((uint32_t)r[11] << 8) | ((uint32_t)r[12] << 16);
                const uint32_t bank = (end >> 20) & 0xF;
                if (bank > 1) return fail("multisound uses an external bank");
                s.top = sp[2];
                s.pan = sp[3];
                s.tune = s8(sp + 4);
                s.root = sp[5] & 0x7F;
                /* Sample record bit 2 is the loop flag; the split's root byte
                 * bit 7 is NT (root := played key). */
                s.flags = static_cast<uint8_t>(((r[9] >> 2) & 1) | ((sp[5] & 0x80) ? 2 : 0));
                s.level = s8(sp + 6);
                s.cutoff = s8(sp + 7);
                s.decay = s8(sp + 8);
                const uint32_t addr1 = (bank << 20) | (start & 0xFFFFF);
                const uint32_t addr2 = (bank << 20) | (loop & 0xFFFFF);
                const uint32_t addr3 = (bank << 20) | (end & 0xFFFFF);
                s.start = addr1 + (uint32_t)waveOffset;
                s.loop = addr2 + (uint32_t)waveOffset;
                s.end = addr3 + (uint32_t)waveOffset;
                if (!(s.start <= s.end && s.end < kPcmWords && s.loop <= s.end))
                    return fail("multisound address outside the PCM");
                splits_.push_back(s);
            }
        }
        return true;
    }

    bool readDrums() {
        drums_.assign(45, m1::DrumSound());
        for (int i = 0; i < 44; ++i) {
            const uint8_t* r = ds(static_cast<uint16_t>(0x5B9E + 14 * i));
            if (!r) return fail("drum record outside the OS image");
            m1::DrumSound& d = drums_[(size_t)i + 1];
            d.tune128 = s16(r);
            const uint32_t start = (uint32_t)r[2] | ((uint32_t)r[3] << 8) | ((uint32_t)r[4] << 16);
            const uint32_t loop = (uint32_t)r[6] | ((uint32_t)r[7] << 8) | ((uint32_t)r[8] << 16);
            d.flags = (r[9] & 4) ? 1 : 0;
            const uint32_t end = (uint32_t)r[10] | ((uint32_t)r[11] << 8) | ((uint32_t)r[12] << 16);
            const uint32_t bank = (end >> 20) & 0xF;
            if (bank > 1) return fail("drum sound uses an external bank");
            d.start = (bank << 20) | (start & 0xFFFFF);
            d.loop = (bank << 20) | (loop & 0xFFFFF);
            d.end = (bank << 20) | (end & 0xFFFFF);
            if (!(d.start <= d.end && d.end < kPcmWords))
                return fail("drum sound address outside the PCM");
            if (d.flags && d.loop > d.end) return fail("drum sound loop past the end");
        }
        return true;
    }

    /* Optional ALL DATA dump: 7-bit packed, the layout of the owner's manual
     * section for MIDI note 12. A wrong dump is ignored, not fatal. */
    void loadPreload(const char* name) {
        if (!name) return;
        char path[512];
        std::vector<uint8_t> syx;
        if (!join(path, sizeof path, name) || !readWhole(path, syx)) return;
        if (syx.size() < 16 || syx[0] != 0xF0 || syx[1] != 0x42 ||
            (syx[2] & 0xF0) != 0x30 || syx[3] != 0x19 || syx[4] != 0x50)
            return;
        size_t end = 8;
        while (end < syx.size() && syx[end] != 0xF7) ++end;
        if (end >= syx.size()) return;

        std::vector<uint8_t> u;
        u.reserve((end - 8) / 8 * 7);
        for (size_t i = 8; i + 8 <= end; i += 8) {
            const uint8_t mask = syx[i];
            for (size_t j = 1; j <= 7; ++j)
                u.push_back(static_cast<uint8_t>(syx[i + j] | (((mask >> (j - 1)) & 1) << 7)));
        }
        const size_t needed = 13261 + (size_t)m1::kProgLen * m1::kPrograms;
        if (u.size() < needed) return;

        std::memcpy(global_.data(), u.data(), 21);
        for (int k = 0; k < 4; ++k)
            std::memcpy(kits_.data() + (size_t)m1::kKitLen * k, u.data() + 21 + 210 * k, 210);
        for (int k = 0; k < 100; ++k)
            std::memcpy(combis_.data() + (size_t)m1::kCombiLen * k, u.data() + 861 + 124 * k, 124);
        for (int k = 0; k < 100; ++k)
            std::memcpy(programs_.data() + (size_t)m1::kProgLen * k,
                        u.data() + 13261 + (size_t)m1::kProgLen * k, m1::kProgLen);
        hasPrograms_ = true;
    }

    const char* dir_ = nullptr;
    char failure_[96] = {0};
    bool ready_ = false;
    bool hasPrograms_ = false;

    std::vector<int16_t> pcm_;
    std::vector<uint8_t> coef_;
    std::vector<uint8_t> os_;
    std::vector<uint8_t> scratch_;

    std::vector<m1::Multisound> ms_;
    std::vector<m1::Split> splits_;
    std::vector<m1::DrumSound> drums_;

    std::vector<uint16_t> vdf1k_, vdf2k_, egRise_, egFall_, velCurve_, mgFreq_, mgDelay_;
    std::vector<uint16_t> mgIntP_, mgIntF_, pitchS_, pitchF_, panReg_, pan101_, delayStart_;
    std::vector<int16_t> vdf1t0_, vdf2t0_;
    std::vector<int8_t> scale_;
    std::vector<uint8_t> programs_, combis_, kits_, global_;
    std::vector<int16_t> fxMem_;
    m1::FxTables fx_{};
    m1::RomTables tables_{};
};

}  // namespace m1unit
