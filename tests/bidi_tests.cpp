#include <kin/core/bidi.hpp>

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kin::BidiRun;
using kin::TextDirection;

// Hebrew and Arabic letters, spelled out so the source stays readable.
const std::string alef = "\xD7\x90";   // U+05D0
const std::string bet = "\xD7\x91";    // U+05D1
const std::string gimel = "\xD7\x92";  // U+05D2
const std::string ar_ba = "\xD8\xA8";  // U+0628
const std::string ar_one = "\xD9\xA1"; // U+0661 ARABIC-INDIC DIGIT ONE
const std::string rle = "\xE2\x80\xAB";
const std::string pdf = "\xE2\x80\xAC";
const std::string lri = "\xE2\x81\xA6";
const std::string pdi = "\xE2\x81\xA9";

// The runs as "text@level" in display order.
std::vector<std::string> shown(std::string_view line, std::optional<TextDirection> base = std::nullopt) {
    std::vector<std::string> out;
    for (const BidiRun& run : kin::bidi_runs(line, base)) {
        out.push_back(std::string{line.substr(run.begin, run.end - run.begin)} + "@" + std::to_string(run.level));
    }
    return out;
}

using V = std::vector<std::string>;

void test_detection() {
    assert(!kin::has_right_to_left("Hello, world 123"));
    assert(!kin::has_right_to_left("Grüße, Привет, 你好"));
    assert(kin::has_right_to_left("abc " + alef));
    assert(kin::has_right_to_left(ar_ba));
    assert(kin::has_right_to_left(rle + "x" + pdf));

    assert(kin::first_strong_direction("123 abc") == TextDirection::LeftToRight);
    assert(kin::first_strong_direction("123 " + alef + " abc") == TextDirection::RightToLeft);
    assert(kin::first_strong_direction("123", TextDirection::RightToLeft) == TextDirection::RightToLeft);
    assert(kin::first_strong_direction(lri + "abc" + pdi + alef) == TextDirection::RightToLeft); // isolates skipped
}

void test_runs() {
    assert(kin::bidi_runs("").empty());
    assert((shown("Hello") == V{"Hello@0"}));

    // Right-to-left words inside left-to-right text keep their place.
    assert((shown("abc " + alef + bet + " def") == V{"abc @0", alef + bet + "@1", " def@0"}));
    // Two right-to-left words in a row: shown in reverse, with the space between them.
    assert((shown("abc " + alef + " " + bet + " def") == V{"abc @0", alef + " " + bet + "@1", " def@0"}));

    // A right-to-left paragraph: the runs go right to left.
    const auto rtl = kin::bidi_runs(alef + bet + " abc " + gimel);
    assert(rtl.front().level == 1 && rtl.back().level == 1);
    {
        // gimel shows leftmost, then abc, then alef-bet rightmost.
        const std::string line = alef + bet + " abc " + gimel;
        std::string visual;
        for (const BidiRun& run : rtl) visual += line.substr(run.begin, run.end - run.begin) + "|";
        assert(visual.find(gimel) < visual.find("abc"));
        assert(visual.find("abc") < visual.find(alef));
    }

    // Numbers in Hebrew text stay left to right (EN at level 2), with their sign.
    {
        const auto runs = kin::bidi_runs(alef + " 12.5% " + bet);
        bool number = false;
        for (const BidiRun& run : runs) {
            const std::string text = (alef + " 12.5% " + bet).substr(run.begin, run.end - run.begin);
            if (text == "12.5%") {
                assert(run.level == 2);
                number = true;
            }
        }
        assert(number);
    }

    // Arabic digits after Arabic letters are AN, also level 2.
    {
        const std::string line = ar_ba + " " + ar_one + ar_one;
        const auto runs = kin::bidi_runs(line);
        assert(runs.size() == 2);
        assert(line.substr(runs[0].begin, runs[0].end - runs[0].begin) == ar_one + ar_one && runs[0].level == 2);
    }

    // European digits after Arabic letters become AN too (W2).
    {
        const std::string line = ar_ba + " 42";
        const auto runs = kin::bidi_runs(line);
        assert(runs.size() == 2 && line.substr(runs[0].begin) == "42" && runs[0].level == 2);
    }

    // Trailing whitespace takes the paragraph level (L1).
    {
        const std::string line = "abc " + alef + "  ";
        const auto runs = kin::bidi_runs(line);
        assert(line.substr(runs.back().begin) == "  " && runs.back().level == 0);
    }

    // The base direction can be given.
    assert((shown("abc", TextDirection::RightToLeft) == V{"abc@2"}));
    assert((shown("123", TextDirection::RightToLeft) == V{"123@2"}));

    // An explicit embedding: abc embedded right to left inside left-to-right text.
    {
        const std::string line = "x " + rle + "ab" + pdf + " y";
        const auto runs = kin::bidi_runs(line);
        bool embedded = false;
        for (const BidiRun& run : runs) {
            if (line.substr(run.begin, run.end - run.begin).find("ab") != std::string::npos) {
                embedded = run.level == 2; // L inside an RTL embedding goes up to 2
            }
        }
        assert(embedded);
    }

    // Byte offsets cover the line exactly once.
    {
        const std::string line = "Hi " + alef + bet + " 3 " + ar_ba + ", ok!";
        const auto runs = kin::bidi_runs(line);
        std::size_t covered = 0;
        for (const BidiRun& run : runs) covered += run.end - run.begin;
        assert(covered == line.size());
    }
}

} // namespace

int main() {
    test_detection();
    test_runs();
    return 0;
}
