// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Dataset scorer / reference comparison for VowelDetector. See CMakeLists.txt.

#include <lipsync_dsp/vowel_detector.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using stackchan::lipsync::Vowel;
using stackchan::lipsync::VowelDetector;
using stackchan::lipsync::VowelResult;

namespace {

constexpr std::size_t kHop = VowelDetector::kChunkSamples;
constexpr std::size_t kWin16 = VowelDetector::kWin * 2;  // analysis window in 16 kHz samples
const char* const kVowelNames[5] = {"a", "i", "u", "e", "o"};
const int kGroup[5] = {0, 1, 2, 1, 2};  // a / i,e / u,o

bool read_wav(const std::string& path, std::vector<std::int16_t>& pcm)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0) return false;
    std::size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const std::uint32_t len = *reinterpret_cast<const std::uint32_t*>(&data[pos + 4]);
        if (std::memcmp(&data[pos], "data", 4) == 0) {
            const std::size_t n = std::min<std::size_t>(len, data.size() - pos - 8) / 2;
            pcm.resize(n);
            std::memcpy(pcm.data(), &data[pos + 8], n * 2);
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

struct Lab { double a, b; std::string l; };

std::vector<Lab> read_lab(const std::string& path)
{
    std::vector<Lab> out;
    std::ifstream f(path);
    Lab l;
    while (f >> l.a >> l.b >> l.l) out.push_back(l);
    return out;
}

const Lab* label_at(const std::vector<Lab>& labs, double t)
{
    for (const auto& l : labs) {
        if (l.a <= t && t < l.b) return &l;
    }
    return nullptr;
}

int vowel_index(const std::string& l)
{
    for (int i = 0; i < 5; ++i) {
        if (l == kVowelNames[i]) return i;
    }
    return -1;
}

std::vector<std::string> list_ids(const std::string& ds)
{
    // index.json is a list of {"id": "...", ...}; a tiny scan is enough.
    std::ifstream f(ds + "/index.json");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::string> ids;
    std::size_t pos = 0;
    while ((pos = s.find("\"id\"", pos)) != std::string::npos) {
        const std::size_t q1 = s.find('"', s.find(':', pos) + 1);
        const std::size_t q2 = s.find('"', q1 + 1);
        ids.push_back(s.substr(q1 + 1, q2 - q1 - 1));
        pos = q2;
    }
    return ids;
}

int score(const std::string& ds, const std::vector<std::string>& ids)
{
    std::array<std::array<long, 6>, 5> conf{};  // [truth][pred], 5 = none
    long sil = 0, sil_false = 0;
    std::map<std::string, std::pair<long, long>> per_speaker;
    for (const auto& id : ids) {
        std::vector<std::int16_t> pcm;
        if (!read_wav(ds + "/" + id + ".wav", pcm)) {
            std::fprintf(stderr, "cannot read %s\n", id.c_str());
            return 1;
        }
        const auto labs = read_lab(ds + "/" + id + ".lab");
        const std::string speaker = id.substr(0, id.rfind('_'));
        auto& ps = per_speaker[speaker];
        VowelDetector det;
        for (std::size_t i = 0; (i + 1) * kHop <= pcm.size(); ++i) {
            const VowelResult r = det.push(&pcm[i * kHop]);
            const double t = static_cast<double>((i + 1) * kHop - kWin16) / 16000.0;
            const Lab* lab = label_at(labs, t);
            const std::string l = lab ? lab->l : "sil";
            const double into = lab ? t - lab->a : 0.0, left = lab ? lab->b - t : 0.0;
            const int truth = vowel_index(l);
            if (truth >= 0 && into > 0.02 && left > 0.02) {
                const int pred = r.vowel ? static_cast<int>(*r.vowel) : 5;
                ++conf[truth][pred];
                ++ps.second;
                if (pred == truth) ++ps.first;
            } else if (l == "sil" && into > 0.05 && left > 0.05) {
                ++sil;
                if (r.vowel) ++sil_false;
            }
        }
    }
    long tot = 0, hit = 0, hit3 = 0;
    for (int v = 0; v < 5; ++v) {
        for (int w = 0; w < 6; ++w) {
            tot += conf[v][w];
            if (w < 5 && kGroup[v] == kGroup[w]) hit3 += conf[v][w];
        }
        hit += conf[v][v];
    }
    std::printf("vowel frames: %ld  accuracy: %.3f  mouth-shape (a / i,e / u,o): %.3f\n", tot,
                static_cast<double>(hit) / std::max(tot, 1L), static_cast<double>(hit3) / std::max(tot, 1L));
    std::printf("silence frames: %ld  false vowel: %.3f\n", sil, static_cast<double>(sil_false) / std::max(sil, 1L));
    std::printf("confusion (rows=truth, cols=pred):\n          a     i     u     e     o  none\n");
    for (int v = 0; v < 5; ++v) {
        long s = 0;
        for (int w = 0; w < 6; ++w) s += conf[v][w];
        std::printf("%4s ", kVowelNames[v]);
        for (int w = 0; w < 6; ++w) std::printf("%6.2f", static_cast<double>(conf[v][w]) / std::max(s, 1L));
        std::printf("   n=%ld\n", s);
    }
    std::printf("per speaker:");
    for (const auto& [sp, hn] : per_speaker) {
        std::printf(" %s=%.3f", sp.c_str(), static_cast<double>(hn.first) / std::max(hn.second, 1L));
    }
    std::printf("\n");
    return 0;
}

// Compare per-frame features / outputs against proto.py --dump-feat output:
// one line per chunk: t voiced f1 f2 open form feat[27] probs[5]
int compare(const std::string& ds, const std::string& id, const std::string& dump)
{
    std::vector<std::int16_t> pcm;
    if (!read_wav(ds + "/" + id + ".wav", pcm)) return 1;
    std::ifstream f(dump);
    if (!f) return 1;
    VowelDetector det;
    double max_feat = 0.0, max_prob = 0.0, max_open = 0.0;
    long frames = 0, voiced_mismatch = 0, f1_bad = 0;
    std::string line;
    for (std::size_t i = 0; (i + 1) * kHop <= pcm.size() && std::getline(f, line); ++i) {
        const VowelResult r = det.push(&pcm[i * kHop]);
        std::istringstream ss(line);
        double t, open, form, f1, f2;
        int voiced;
        ss >> t >> voiced >> f1 >> f2 >> open >> form;
        std::array<double, VowelDetector::kFeatures> feat{};
        for (auto& v : feat) ss >> v;
        std::array<double, 5> probs{};
        for (auto& v : probs) ss >> v;
        ++frames;
        if (voiced != static_cast<int>(r.voiced)) ++voiced_mismatch;
        if (voiced && r.voiced) {
            for (std::size_t k = 0; k < VowelDetector::kFeatures; ++k) {
                max_feat = std::max(max_feat, std::fabs(feat[k] - det.last_features()[k]));
            }
            for (std::size_t k = 0; k < 5; ++k) max_prob = std::max(max_prob, std::fabs(probs[k] - r.probs[k]));
            max_open = std::max(max_open, std::fabs(open - r.open));
            if (f1 > 0 && std::fabs(f1 - r.f1) > 5.0) ++f1_bad;
        }
    }
    std::printf("frames=%ld voiced_mismatch=%ld f1_diff>5Hz=%ld max|dfeat|=%.4f max|dprob|=%.4f max|dopen|=%.4f\n",
                frames, voiced_mismatch, f1_bad, max_feat, max_prob, max_open);
    return (voiced_mismatch * 100 > frames * 2 || max_prob > 0.05) ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <dataset-dir> [--compare <id> <dump.tsv>] [--speaker <name>]\n", argv[0]);
        return 2;
    }
    const std::string ds = argv[1];
    if (argc >= 5 && std::strcmp(argv[2], "--compare") == 0) return compare(ds, argv[3], argv[4]);
    std::vector<std::string> ids = list_ids(ds);
    if (argc >= 4 && std::strcmp(argv[2], "--speaker") == 0) {
        std::vector<std::string> sel;
        for (const auto& id : ids) {
            if (id.rfind(std::string(argv[3]) + "_", 0) == 0) sel.push_back(id);
        }
        ids = sel;
    }
    return score(ds, ids);
}
