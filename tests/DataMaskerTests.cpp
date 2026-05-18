#include <gtest/gtest.h>
#include "DataMasker.h"
#include <string>
#include <vector>
#include <map>

// ── MaskJson ──────────────────────────────────────────────────────────────────

TEST(DataMasker, EmptyJson_Passthrough) {
    EXPECT_EQ(MaskJson("", {"password"}), "");
}

TEST(DataMasker, EmptyKeywords_Passthrough) {
    std::string json = R"({"password":"secret"})";
    EXPECT_EQ(MaskJson(json, {}), json);
}

TEST(DataMasker, NoMatch_Unchanged) {
    std::string json = R"({"username":"john","email":"john@example.com"})";
    EXPECT_EQ(MaskJson(json, {"password"}), json);
}

TEST(DataMasker, StringValue_SameLengthStars) {
    std::string json   = R"({"password":"secret"})";
    std::string expect = R"({"password":"******"})";
    EXPECT_EQ(MaskJson(json, {"password"}), expect);
}

TEST(DataMasker, CaseInsensitiveKeyMatch) {
    std::string json   = R"({"PASSWORD":"secret"})";
    std::string expect = R"({"PASSWORD":"******"})";
    EXPECT_EQ(MaskJson(json, {"password"}), expect);
}

TEST(DataMasker, NestedObject_MaskedDeep) {
    std::string json   = R"({"user":{"name":"John","password":"secret123"}})";
    std::string expect = R"({"user":{"name":"John","password":"*********"}})";
    EXPECT_EQ(MaskJson(json, {"password"}), expect);
}

TEST(DataMasker, ArrayOfObjects_AllMatchesMasked) {
    std::string json   = R"([{"id":1,"password":"abc"},{"id":2,"password":"xyz"}])";
    std::string expect = R"([{"id":1,"password":"***"},{"id":2,"password":"***"}])";
    EXPECT_EQ(MaskJson(json, {"password"}), expect);
}

TEST(DataMasker, ObjectValue_SingleStar) {
    std::string json   = R"({"creditCard":{"number":"4111","cvv":"123"}})";
    std::string expect = R"({"creditCard":"*"})";
    EXPECT_EQ(MaskJson(json, {"creditCard"}), expect);
}

TEST(DataMasker, NumericValue_QuotedStars) {
    std::string json   = R"({"ssn":123456789})";
    std::string expect = R"({"ssn":"*********"})";
    EXPECT_EQ(MaskJson(json, {"ssn"}), expect);
}

TEST(DataMasker, BooleanValue_QuotedStars) {
    std::string json   = R"({"secret":true})";
    std::string expect = R"({"secret":"****"})";
    EXPECT_EQ(MaskJson(json, {"secret"}), expect);
}

TEST(DataMasker, MultipleKeywords_BothMasked) {
    std::string json   = R"({"password":"abc","ssn":"123","name":"John"})";
    std::string expect = R"({"password":"***","ssn":"***","name":"John"})";
    EXPECT_EQ(MaskJson(json, {"password", "ssn"}), expect);
}

TEST(DataMasker, SamplePayload_MaskedField) {
    // Based on the real sample payload body
    std::string json   = R"({"name":"string","maskedField":"string","paymentType":"string"})";
    std::string expect = R"({"name":"string","maskedField":"******","paymentType":"string"})";
    EXPECT_EQ(MaskJson(json, {"maskedField"}), expect);
}

TEST(DataMasker, Over500KB_NotMasked) {
    // Payload larger than 500 KB must pass through unmodified
    std::string big = "{\"password\":\"" + std::string(501 * 1024, 'x') + "\"}";
    EXPECT_EQ(MaskJson(big, {"password"}), big);
}

// ── MaskHeaders ───────────────────────────────────────────────────────────────

TEST(DataMasker, MaskHeaders_MatchingKey_AllStars) {
    std::map<std::string, std::string> hdrs = {{"authorization", "Bearer token123"}};
    MaskHeaders(hdrs, {"authorization"});
    EXPECT_EQ(hdrs["authorization"], std::string(14, '*'));
}

TEST(DataMasker, MaskHeaders_CaseInsensitive) {
    std::map<std::string, std::string> hdrs = {{"Authorization", "Bearer token"}};
    MaskHeaders(hdrs, {"authorization"});
    EXPECT_EQ(hdrs["Authorization"], std::string(12, '*'));
}

TEST(DataMasker, MaskHeaders_NoMatch_Unchanged) {
    std::map<std::string, std::string> hdrs = {{"content-type", "application/json"}};
    MaskHeaders(hdrs, {"authorization"});
    EXPECT_EQ(hdrs["content-type"], "application/json");
}

TEST(DataMasker, MaskHeaders_EmptyKeywords_Unchanged) {
    std::map<std::string, std::string> hdrs = {{"authorization", "Bearer token"}};
    MaskHeaders(hdrs, {});
    EXPECT_EQ(hdrs["authorization"], "Bearer token");
}
