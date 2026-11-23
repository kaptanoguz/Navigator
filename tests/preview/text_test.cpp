// tests/preview/text_test.cpp: looks_binary() and decode_text(): UTF-8 repair, BOMs, UTF-16,
// line terminators, long lines split into rows, truncation.
#include <catch2/catch_test_macros.hpp>

#include <navigator/preview.hpp>

using namespace navigator;
using namespace navigator::preview;

namespace {

Span<const std::uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

TextPreview decode(std::string_view s, TextOptions opt = {}, std::uint64_t file_size = 0) {
    auto r = decode_text(bytes(s), file_size == 0 ? s.size() : file_size, opt);
    REQUIRE(r);
    return std::move(*r);
}

} // namespace

TEST_CASE("looks_binary detects NULs unless a UTF-16 BOM starts the data", "[text]") {
    CHECK_FALSE(looks_binary(bytes("hello world\n")));
    CHECK(looks_binary(bytes(std::string_view("ab\0cd", 5))));
    CHECK_FALSE(looks_binary(bytes(std::string_view("\xFF\xFE"
                                                    "a\0b\0",
                                                    6))));
    CHECK_FALSE(looks_binary(bytes(std::string_view("\xFE\xFF\0a\0b", 6))));
    std::string late(kBinarySniffBytes + 10, 'a');
    late[kBinarySniffBytes + 5] = '\0';
    CHECK_FALSE(looks_binary(bytes(late))); // only the sniff window counts
}

TEST_CASE("decode_text splits lines on LF, CRLF and CR", "[text]") {
    const TextPreview t = decode("one\ntwo\r\nthree\rfour");
    CHECK(t.encoding == TextEncoding::Utf8);
    REQUIRE(t.rows.size() == 4);
    CHECK(t.line_count == 4);
    CHECK(t.row_text(0) == "one");
    CHECK(t.row_text(1) == "two");
    CHECK(t.row_text(2) == "three");
    CHECK(t.row_text(3) == "four");
    CHECK(t.text == "one\ntwo\r\nthree\rfour"); // terminators kept
    CHECK(t.longest_line_bytes == 5);
    CHECK_FALSE(t.truncated);
    CHECK(t.bytes_read == t.file_size);

    const TextPreview blank = decode("a\n\nb\n");
    REQUIRE(blank.rows.size() == 3);
    CHECK(blank.row_text(1).empty());
    CHECK(blank.rows[1].line == 1);

    const TextPreview empty = decode("");
    CHECK(empty.rows.empty());
    CHECK(empty.line_count == 0);
}

TEST_CASE("decode_text repairs invalid UTF-8 with U+FFFD", "[text]") {
    const TextPreview t = decode("a\xFF"
                                 "b\xC3(c\xE2\x82\n");
    CHECK(t.text == "a\xEF\xBF\xBD"
                    "b\xEF\xBF\xBD(c\xEF\xBF\xBD\n");
    CHECK(t.replaced_sequences == 3);
    // Overlong and surrogate encodings are invalid.
    const TextPreview o = decode("\xC0\xAF\xED\xA0\x80");
    CHECK(o.replaced_sequences >= 3);
    // Valid multi-byte text passes unchanged.
    const TextPreview ok = decode("İstanbul ığüşöç 日本 \xF0\x9F\x98\x80");
    CHECK(ok.text == "İstanbul ığüşöç 日本 \xF0\x9F\x98\x80");
    CHECK(ok.replaced_sequences == 0);
}

TEST_CASE("decode_text strips a UTF-8 BOM and converts UTF-16 LE/BE", "[text]") {
    const TextPreview bom = decode("\xEF\xBB\xBFhi\n");
    CHECK(bom.encoding == TextEncoding::Utf8Bom);
    CHECK(bom.text == "hi\n");

    const std::string le("\xFF\xFE"
                         "h\0i\0\n\0\x1F\x01",
                         10); // "hi\nğ"
    const TextPreview l = decode(le);
    CHECK(l.encoding == TextEncoding::Utf16Le);
    CHECK(l.text == "hi\n\xC4\x9F");
    REQUIRE(l.rows.size() == 2);

    const std::string be("\xFE\xFF\0h\0i\xD8\x3D\xDE\x00", 10); // "hi" + U+1F600
    const TextPreview b = decode(be);
    CHECK(b.encoding == TextEncoding::Utf16Be);
    CHECK(b.text == "hi\xF0\x9F\x98\x80");

    const std::string lone("\xFF\xFE\x00\xD8"
                           "a\0",
                           6); // unpaired high surrogate
    const TextPreview u = decode(lone);
    CHECK(u.text == "\xEF\xBF\xBD"
                    "a");
    CHECK(u.replaced_sequences == 1);
}

TEST_CASE("decode_text splits long lines into bounded rows on UTF-8 boundaries", "[text]") {
    TextOptions opt;
    opt.max_row_bytes = 64;
    std::string line;
    for (int i = 0; i < 100; ++i) {
        line += "ğ"; // 2 bytes each: 200 bytes
    }
    line += "x";
    const TextPreview t = decode(line + "\nend", opt);
    CHECK(t.line_count == 2);
    CHECK(t.longest_line_bytes == 201);
    std::size_t total = 0;
    std::size_t rows_line0 = 0;
    for (std::size_t i = 0; i < t.rows.size(); ++i) {
        CHECK(t.rows[i].length <= 64);
        const std::string_view r = t.row_text(i);
        if (!r.empty()) {
            CHECK((static_cast<unsigned char>(r.front()) & 0xC0U) != 0x80U); // never mid-sequence
        }
        if (t.rows[i].line == 0) {
            total += t.rows[i].length;
            ++rows_line0;
        }
    }
    CHECK(total == 201);
    CHECK(rows_line0 == 4);
    CHECK(t.row_text(t.rows.size() - 1) == "end");

    // A 4 MiB single line costs 4096 rows of 1 KiB.
    const std::string huge(std::size_t{4} << 20U, 'a');
    const TextPreview h = decode(huge);
    CHECK(h.rows.size() == 4096);
    CHECK(h.line_count == 1);
}

TEST_CASE("decode_text truncates by bytes and rows", "[text]") {
    TextOptions opt;
    opt.max_bytes = 6;
    // "abcdé" is 6 bytes ("é" = C3 A9); the cap cuts inside the next sequence.
    const TextPreview t = decode("abcd\xC3\xA9\xC3\xA9", opt);
    CHECK(t.text == "abcd\xC3\xA9");
    CHECK(t.truncated);
    CHECK(t.replaced_sequences == 0);

    opt.max_bytes = 5; // cuts "é" in half: dropped, not replaced
    const TextPreview cut = decode("abcd\xC3\xA9", opt);
    CHECK(cut.text == "abcd");
    CHECK(cut.replaced_sequences == 0);
    CHECK(cut.truncated);

    // The caller read only a prefix of a larger file.
    const TextPreview prefix = decode("abc", TextOptions{}, 1000);
    CHECK(prefix.truncated);
    CHECK(prefix.file_size == 1000);

    TextOptions rows;
    rows.max_rows = 2;
    const TextPreview r = decode("1\n2\n3\n4\n", rows);
    CHECK(r.rows.size() == 2);
    CHECK(r.truncated);
    CHECK(r.line_count == 2);

    // A genuinely incomplete sequence at the real end of the file is replaced.
    const TextPreview end = decode("ab\xC3");
    CHECK(end.text == "ab\xEF\xBF\xBD");
}

TEST_CASE("decode_text rejects binary data and bad options", "[text]") {
    CHECK(decode_text(bytes(std::string_view("a\0b", 3)), 3, {}).error().code ==
          ErrorCode::Unsupported);
    TextOptions bad;
    bad.max_row_bytes = 10;
    CHECK(decode_text(bytes("abc"), 3, bad).error().code == ErrorCode::InvalidArgument);
    bad = {};
    bad.max_rows = 0;
    CHECK(decode_text(bytes("abc"), 3, bad).error().code == ErrorCode::InvalidArgument);
    bad = {};
    bad.max_bytes = 0;
    CHECK(decode_text(bytes("abc"), 3, bad).error().code == ErrorCode::InvalidArgument);
}
