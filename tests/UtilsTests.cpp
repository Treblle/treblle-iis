#include <gtest/gtest.h>
#include "Utils.h"
#include <string>
#include <map>

// ── ToLower ───────────────────────────────────────────────────────────────────

TEST(Utils, ToLower_MixedCase) {
    EXPECT_EQ(ToLower("Hello World"), "hello world");
}

TEST(Utils, ToLower_AlreadyLower_Unchanged) {
    EXPECT_EQ(ToLower("treblle"), "treblle");
}

TEST(Utils, ToLower_AllUpper) {
    EXPECT_EQ(ToLower("TREBLLE"), "treblle");
}

TEST(Utils, ToLower_Empty) {
    EXPECT_EQ(ToLower(""), "");
}

// ── StartsWithCI ──────────────────────────────────────────────────────────────

TEST(Utils, StartsWithCI_ExactMatch) {
    EXPECT_TRUE(StartsWithCI("/api/users", "/api"));
}

TEST(Utils, StartsWithCI_CaseInsensitive) {
    EXPECT_TRUE(StartsWithCI("/API/users", "/api"));
    EXPECT_TRUE(StartsWithCI("/api/users", "/API"));
}

TEST(Utils, StartsWithCI_NoMatch) {
    EXPECT_FALSE(StartsWithCI("/health", "/api"));
}

TEST(Utils, StartsWithCI_EmptyPrefix_AlwaysTrue) {
    EXPECT_TRUE(StartsWithCI("/anything", ""));
    EXPECT_TRUE(StartsWithCI("", ""));
}

TEST(Utils, StartsWithCI_PrefixLongerThanStr) {
    EXPECT_FALSE(StartsWithCI("/a", "/api/v1/users"));
}

// ── JsonEscape ────────────────────────────────────────────────────────────────

TEST(Utils, JsonEscape_Quote) {
    EXPECT_EQ(JsonEscape("say \"hello\""), "say \\\"hello\\\"");
}

TEST(Utils, JsonEscape_Backslash) {
    EXPECT_EQ(JsonEscape("C:\\Users"), "C:\\\\Users");
}

TEST(Utils, JsonEscape_Newline) {
    EXPECT_EQ(JsonEscape("line1\nline2"), "line1\\nline2");
}

TEST(Utils, JsonEscape_CarriageReturn) {
    EXPECT_EQ(JsonEscape("line1\rline2"), "line1\\rline2");
}

TEST(Utils, JsonEscape_Tab) {
    EXPECT_EQ(JsonEscape("col1\tcol2"), "col1\\tcol2");
}

TEST(Utils, JsonEscape_ControlChar_HexEscape) {
    std::string s(1, '\x01');
    EXPECT_EQ(JsonEscape(s), "\\u0001");
}

TEST(Utils, JsonEscape_NoSpecials_Unchanged) {
    EXPECT_EQ(JsonEscape("normal text 123"), "normal text 123");
}

TEST(Utils, JsonEscape_Empty) {
    EXPECT_EQ(JsonEscape(""), "");
}

// ── ParseQueryPath ────────────────────────────────────────────────────────────

TEST(Utils, ParseQueryPath_WithQuery_StripsQuery) {
    EXPECT_EQ(ParseQueryPath("/api/test?foo=bar&baz=1"), "/api/test");
}

TEST(Utils, ParseQueryPath_NoQuery_Unchanged) {
    EXPECT_EQ(ParseQueryPath("/api/test"), "/api/test");
}

TEST(Utils, ParseQueryPath_Empty) {
    EXPECT_EQ(ParseQueryPath(""), "");
}

TEST(Utils, ParseQueryPath_QuestionMarkOnly) {
    EXPECT_EQ(ParseQueryPath("/path?"), "/path");
}

// ── ParseQueryString ──────────────────────────────────────────────────────────

TEST(Utils, ParseQueryString_SingleParam) {
    auto r = ParseQueryString("/path?key=value");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r.at("key"), "value");
}

TEST(Utils, ParseQueryString_MultipleParams) {
    auto r = ParseQueryString("/path?a=1&b=2&c=3");
    EXPECT_EQ(r.size(), 3u);
    EXPECT_EQ(r.at("a"), "1");
    EXPECT_EQ(r.at("b"), "2");
    EXPECT_EQ(r.at("c"), "3");
}

TEST(Utils, ParseQueryString_NoQuery_EmptyMap) {
    EXPECT_TRUE(ParseQueryString("/path").empty());
}

TEST(Utils, ParseQueryString_KeyWithoutValue) {
    auto r = ParseQueryString("/path?flag");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r.at("flag"), "");
}

// ── ComputeHostId ─────────────────────────────────────────────────────────────

TEST(Utils, ComputeHostId_Deterministic) {
    EXPECT_EQ(ComputeHostId("localhost:8081"), ComputeHostId("localhost:8081"));
}

TEST(Utils, ComputeHostId_DifferentInputs_DifferentIds) {
    EXPECT_NE(ComputeHostId("localhost"), ComputeHostId("example.com"));
}

TEST(Utils, ComputeHostId_UUIDFormat) {
    std::string id = ComputeHostId("localhost:8081");
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx  (8-4-4-4-12 hex chars)
    EXPECT_EQ(id.size(), 36u);
    EXPECT_EQ(id[8],  '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_EQ(id[18], '-');
    EXPECT_EQ(id[23], '-');
}

TEST(Utils, ComputeHostId_SameHostDifferentPorts_DifferentIds) {
    EXPECT_NE(ComputeHostId("api.example.com:8080"), ComputeHostId("api.example.com:9090"));
}

TEST(Utils, ComputeHostId_HostWithoutPort_StableId) {
    EXPECT_EQ(ComputeHostId("api.example.com"), ComputeHostId("api.example.com"));
}

TEST(Utils, ComputeHostId_HostWithPortVsWithout_DifferentIds) {
    EXPECT_NE(ComputeHostId("api.example.com"), ComputeHostId("api.example.com:8080"));
}
