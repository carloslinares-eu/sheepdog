#include "core/platform/fs.hpp"
#include "core/platform/native_path.hpp"
#include "core/platform/result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using sheepdog::platform::display_string;
using sheepdog::platform::is_valid_utf8;
using sheepdog::platform::NativePath;
using sheepdog::platform::utf16_from_wtf8;
using sheepdog::platform::wtf8_from_utf16;

namespace {
const std::string kReplacement = "\xEF\xBF\xBD"; // U+FFFD in UTF-8
}

TEST_CASE("WTF-8 round-trips UTF-16, including unpaired surrogates", "[unit]") {
    const std::u16string samples[] = {
        u"",
        u"plain.txt",
        u"K\u00e4se \u00fc\u00df",                        // Latin-1 range
        u"\u65e5\u672c\u8a9e.jpg",                        // CJK
        u"\U0001F600 smile",                              // a surrogate pair (U+1F600)
        std::u16string(1, u'\xD800'),                     // lone lead surrogate
        std::u16string(1, u'\xDFFF'),                     // lone trail surrogate
        std::u16string{u'a', u'\xDC00', u'\xD83D', u'b'}, // trail then lead: two lone surrogates
        std::u16string{u'\xD83D', u'\xD83D', u'\xDE00'},  // lone lead, then a valid pair
    };
    for (const auto& s : samples) {
        const std::string wtf8 = wtf8_from_utf16(s);
        const auto back = utf16_from_wtf8(wtf8);
        REQUIRE(back.has_value());
        CHECK(*back == s);
    }
}

TEST_CASE("WTF-8 encodes pairs as 4 bytes and lone surrogates as 3", "[unit]") {
    CHECK(wtf8_from_utf16(u"\U0001F600") == "\xF0\x9F\x98\x80");
    CHECK(wtf8_from_utf16(std::u16string(1, u'\xD800')) == "\xED\xA0\x80");
    CHECK(wtf8_from_utf16(u"\u00e4") == "\xC3\xA4");
}

TEST_CASE("WTF-8 decoding rejects bytes that are not WTF-8", "[unit]") {
    CHECK_FALSE(utf16_from_wtf8("\xFF").has_value());
    CHECK_FALSE(utf16_from_wtf8("abc\x80").has_value());          // stray continuation byte
    CHECK_FALSE(utf16_from_wtf8("\xC0\xAF").has_value());         // overlong '/'
    CHECK_FALSE(utf16_from_wtf8("\xE0\x80\xAF").has_value());     // overlong '/'
    CHECK_FALSE(utf16_from_wtf8("\xF4\x90\x80\x80").has_value()); // above U+10FFFF
    CHECK_FALSE(utf16_from_wtf8("\xE2\x82").has_value());         // truncated
}

TEST_CASE("display_string replaces invalid bytes and surrogates with U+FFFD", "[unit]") {
    CHECK(display_string(std::string_view("ok.txt")) == "ok.txt");
    CHECK(display_string(std::string_view("K\xC3\xA4se")) == "K\xC3\xA4se");
    CHECK(display_string(std::string_view("bad\xFF.txt")) == "bad" + kReplacement + ".txt");
    CHECK(display_string(std::string_view("\xED\xA0\x80x")) == kReplacement + "x");
    CHECK(display_string(std::string_view("\xC3")) == kReplacement);
    const std::string shown = display_string(std::string_view("a\xFE\xFF"
                                                              "b"));
    CHECK(shown == "a" + kReplacement + kReplacement + "b");
    CHECK(is_valid_utf8(shown));
}

TEST_CASE("is_valid_utf8 accepts UTF-8 and refuses surrogates", "[unit]") {
    CHECK(is_valid_utf8("plain"));
    CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));
    CHECK_FALSE(is_valid_utf8("\xFF"));
}

TEST_CASE("NativePath joins names and splits off the last one", "[unit]") {
    const char sep = sheepdog::platform::kSeparator;
    const NativePath base("root");
    const NativePath joined = base / "a" / "b.txt";
    CHECK(joined.bytes() == std::string("root") + sep + "a" + sep + "b.txt");
    CHECK(joined.filename() == "b.txt");
    CHECK(joined.parent() == base / "a");
    CHECK((NativePath(std::string("x") + sep) / "y").bytes() == std::string("x") + sep + "y");
    CHECK((NativePath() / "y").bytes() == "y");
#ifdef _WIN32
    CHECK(NativePath(R"(C:\foo)").parent().bytes() == R"(C:\)");
    CHECK(NativePath("C:/a/b").filename() == "b");
#else
    CHECK(NativePath("/foo").parent().bytes() == "/");
#endif
}

TEST_CASE("Error::message names the path", "[unit]") {
    const sheepdog::platform::Error error{
        std::make_error_code(std::errc::no_such_file_or_directory), NativePath("missing\xFF")};
    const std::string text = error.message();
    CHECK(text.find("missing" + kReplacement) != std::string::npos);
    CHECK(sheepdog::platform::is_not_found(error.code));
}

TEST_CASE("swap_ascii_case only touches ASCII letters", "[unit]") {
    CHECK(sheepdog::platform::swap_ascii_case("Foo.TXT 1\xC3\xA4") == "fOO.txt 1\xC3\xA4");
}

#ifdef _WIN32
TEST_CASE("long-path prefix normalises absolute paths", "[unit][win]") {
    using sheepdog::platform::add_long_path_prefix;
    CHECK(add_long_path_prefix(LR"(C:\a\b)") == LR"(\\?\C:\a\b)");
    CHECK(add_long_path_prefix(L"C:/a//b/./c/../d") == LR"(\\?\C:\a\b\d)");
    CHECK(add_long_path_prefix(LR"(C:\)") == LR"(\\?\C:\)");
    CHECK(add_long_path_prefix(LR"(C:\..\x)") == LR"(\\?\C:\x)");
    CHECK(add_long_path_prefix(LR"(\\server\share\a\..\..\b)") == LR"(\\?\UNC\server\share\b)");
    CHECK(add_long_path_prefix(LR"(C:\trailing dot.)") == LR"(\\?\C:\trailing dot.)");
    CHECK(add_long_path_prefix(LR"(\\?\C:\keep\..\as is)") == LR"(\\?\C:\keep\..\as is)");
    CHECK(sheepdog::platform::native_from_wide(LR"(\\?\UNC\srv\sh)").bytes() == R"(\\srv\sh)");
    CHECK(sheepdog::platform::native_from_wide(LR"(\\?\C:\x)").bytes() == R"(C:\x)");
}

TEST_CASE("win32_path makes relative paths absolute and refuses bad bytes", "[unit][win]") {
    const auto rel = sheepdog::platform::win32_path(NativePath("some\\rel"));
    REQUIRE(rel.has_value());
    CHECK(rel->rfind(LR"(\\?\)", 0) == 0);
    CHECK(rel->size() > 12);
    const auto bad = sheepdog::platform::win32_path(NativePath("bad\xFF"));
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == std::errc::illegal_byte_sequence);
}
#endif
