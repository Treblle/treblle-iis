#include <gtest/gtest.h>
#include "BodyCapture.h"
#include <string>

// ── Multipart test helpers ─────────────────────────────────────────────────────

// Build a minimal but spec-correct multipart body string for testing.
static std::string MakeMultipart(const std::string& boundary,
                                  const std::string& fieldName,
                                  const std::string& filename,
                                  const std::string& mimeType,
                                  const std::string& data) {
    return "--" + boundary + "\r\n"
           "Content-Disposition: form-data; name=\"" + fieldName + "\"; filename=\"" + filename + "\"\r\n"
           "Content-Type: " + mimeType + "\r\n"
           "\r\n" +
           data + "\r\n"
           "--" + boundary + "--\r\n";
}

// ── IsLikelyJson ──────────────────────────────────────────────────────────────

// ── ParseMultipartFiles ───────────────────────────────────────────────────────

TEST(Multipart, SingleFile_ExactSize) {
    std::string boundary = "testboundary";
    std::string data     = "fake jpeg bytes";
    std::string body     = MakeMultipart(boundary, "upload", "photo.jpg", "image/jpeg", data);
    std::string result   = ParseMultipartFiles(body, boundary, 9999);

    EXPECT_NE(result.find("\"name\":\"photo.jpg\""),     std::string::npos);
    EXPECT_NE(result.find("\"type\":\"image/jpeg\""),    std::string::npos);
    // Exact size: length of "fake jpeg bytes"
    EXPECT_NE(result.find("\"size\":" + std::to_string(data.size())), std::string::npos);
}

TEST(Multipart, SingleFile_FallsBackToContentLength_WhenDataTruncated) {
    // Body is intentionally cut off before the closing boundary
    std::string boundary = "bound";
    std::string body     = "--bound\r\n"
                           "Content-Disposition: form-data; name=\"f\"; filename=\"big.bin\"\r\n"
                           "Content-Type: application/octet-stream\r\n"
                           "\r\n"
                           "partial data — no closing boundary in buffer";
    std::string result   = ParseMultipartFiles(body, boundary, 1048576);

    EXPECT_NE(result.find("\"name\":\"big.bin\""),              std::string::npos);
    EXPECT_NE(result.find("\"type\":\"application/octet-stream\""), std::string::npos);
    EXPECT_NE(result.find("\"size\":1048576"),                  std::string::npos);
}

TEST(Multipart, MultipleFiles) {
    std::string boundary = "multiboundary";
    std::string body =
        "--multiboundary\r\n"
        "Content-Disposition: form-data; name=\"a\"; filename=\"avatar.png\"\r\n"
        "Content-Type: image/png\r\n"
        "\r\n"
        "pngbytes\r\n"
        "--multiboundary\r\n"
        "Content-Disposition: form-data; name=\"b\"; filename=\"doc.pdf\"\r\n"
        "Content-Type: application/pdf\r\n"
        "\r\n"
        "pdfbytes\r\n"
        "--multiboundary--\r\n";

    std::string result = ParseMultipartFiles(body, boundary, 500);

    EXPECT_NE(result.find("avatar.png"),       std::string::npos);
    EXPECT_NE(result.find("image/png"),        std::string::npos);
    EXPECT_NE(result.find("doc.pdf"),          std::string::npos);
    EXPECT_NE(result.find("application/pdf"), std::string::npos);
}

TEST(Multipart, TextFieldsOnly_NoFiles_EmptyArray) {
    std::string boundary = "b";
    std::string body =
        "--b\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "john\r\n"
        "--b--\r\n";
    EXPECT_EQ(ParseMultipartFiles(body, boundary, 100), "[]");
}

TEST(Multipart, MixedFieldsAndFile_OnlyFileReturned) {
    std::string boundary = "mix";
    std::string body =
        "--mix\r\n"
        "Content-Disposition: form-data; name=\"description\"\r\n"
        "\r\n"
        "some text\r\n"
        "--mix\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"report.xlsx\"\r\n"
        "Content-Type: application/vnd.openxmlformats-officedocument.spreadsheetml.sheet\r\n"
        "\r\n"
        "xlsxdata\r\n"
        "--mix--\r\n";

    std::string result = ParseMultipartFiles(body, boundary, 300);

    EXPECT_NE(result.find("report.xlsx"),   std::string::npos);
    EXPECT_EQ(result.find("description"),   std::string::npos);  // text field not included
}

TEST(Multipart, EmptyBoundary_EmptyArray) {
    EXPECT_EQ(ParseMultipartFiles("anything", "", 0), "[]");
}

TEST(Multipart, FilenameWithSpecialChars_JsonEscaped) {
    std::string boundary = "bnd";
    std::string body     = MakeMultipart(boundary, "f", "my \"file\".txt", "text/plain", "hi");
    std::string result   = ParseMultipartFiles(body, boundary, 50);
    // Quote inside filename must be escaped as \"
    EXPECT_NE(result.find("my \\\"file\\\".txt"), std::string::npos);
}

TEST(Multipart, NoContentType_DefaultsToOctetStream) {
    std::string boundary = "bnd";
    std::string body =
        "--bnd\r\n"
        "Content-Disposition: form-data; name=\"f\"; filename=\"data.bin\"\r\n"
        "\r\n"
        "rawbytes\r\n"
        "--bnd--\r\n";
    std::string result = ParseMultipartFiles(body, boundary, 50);
    EXPECT_NE(result.find("application/octet-stream"), std::string::npos);
}

// ── IsLikelyJson ──────────────────────────────────────────────────────────────

TEST(BodyCapture, IsLikelyJson_EmptyObject_True) {
    EXPECT_TRUE(IsLikelyJson("{}"));
}

TEST(BodyCapture, IsLikelyJson_EmptyArray_True) {
    EXPECT_TRUE(IsLikelyJson("[]"));
}

TEST(BodyCapture, IsLikelyJson_ObjectWithContent_True) {
    EXPECT_TRUE(IsLikelyJson(R"({"name":"string","maskedField":"string"})"));
}

TEST(BodyCapture, IsLikelyJson_ArrayWithContent_True) {
    EXPECT_TRUE(IsLikelyJson("[1, 2, 3]"));
}

TEST(BodyCapture, IsLikelyJson_LeadingTrailingWhitespace_True) {
    EXPECT_TRUE(IsLikelyJson("  { \"key\": \"value\" }  "));
    EXPECT_TRUE(IsLikelyJson("\r\n[ 1, 2 ]\r\n"));
}

TEST(BodyCapture, IsLikelyJson_NestedObject_True) {
    EXPECT_TRUE(IsLikelyJson(R"({"user":{"name":"John","password":"secret"}})"));
}

TEST(BodyCapture, IsLikelyJson_Empty_False) {
    EXPECT_FALSE(IsLikelyJson(""));
}

TEST(BodyCapture, IsLikelyJson_WhitespaceOnly_False) {
    EXPECT_FALSE(IsLikelyJson("   \t\r\n"));
}

TEST(BodyCapture, IsLikelyJson_PlainString_False) {
    EXPECT_FALSE(IsLikelyJson("hello world"));
}

TEST(BodyCapture, IsLikelyJson_XmlLike_False) {
    EXPECT_FALSE(IsLikelyJson("<root><item>value</item></root>"));
}

TEST(BodyCapture, IsLikelyJson_Number_False) {
    EXPECT_FALSE(IsLikelyJson("42"));
}

TEST(BodyCapture, IsLikelyJson_MismatchedBraces_False) {
    // Starts with '{' but ends with ']' — must be rejected
    EXPECT_FALSE(IsLikelyJson("{\"key\":\"value\"]"));
}

TEST(BodyCapture, IsLikelyJson_QuotedString_False) {
    EXPECT_FALSE(IsLikelyJson("\"just a string\""));
}
