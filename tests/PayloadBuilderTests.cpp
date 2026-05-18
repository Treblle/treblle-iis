#include <gtest/gtest.h>
#include "PayloadBuilder.h"
#include <string>

static bool Contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

class PayloadBuilderTest : public ::testing::Test {
protected:
    RequestContext  ctx;
    TreblleConfig   cfg;

    void SetUp() override {
        cfg.apiKey    = "rIdDODfpGjmzllxM92dAJLhJPA";
        cfg.sdkToken  = "PyZL29nBwb0gIFZ2JsqpwQlBWH8UkABf";
        cfg.treblleUrl = "https://ingress.treblle.com";

        ctx.method       = "POST";
        ctx.url          = "http://localhost:8081/api/test/register";
        ctx.routePath    = "/api/test/register";
        ctx.clientIp     = "172.31.82.162";
        ctx.userAgent    = "Mozilla/5.0";
        ctx.protocol     = "HTTP/1.1";
        ctx.internalId   = "7d8e120a-9ea0-ea78-dc27-b6dd61c13667";
        ctx.internalName = "Testing API - IIS";
        ctx.statusCode   = 200;
        ctx.responseSize = 63;
        ctx.requestBody  = R"({"name":"string","maskedField":"secret","paymentType":"visa"})";
        ctx.responseBody = R"({"name":"string","maskedField":"secret","paymentType":"visa"})";
    }
};

TEST_F(PayloadBuilderTest, Build_TopLevelKeys_Present) {
    std::string p = PayloadBuilder::Build(ctx, cfg, 1112.488, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "\"api_key\""));
    EXPECT_TRUE(Contains(p, "\"sdk_token\""));
    EXPECT_TRUE(Contains(p, "\"sdk\""));
    EXPECT_TRUE(Contains(p, "\"version\""));
    EXPECT_TRUE(Contains(p, "\"internal_id\""));
    EXPECT_TRUE(Contains(p, "\"internal_name\""));
    EXPECT_TRUE(Contains(p, "\"data\""));
}

TEST_F(PayloadBuilderTest, Build_ApiKeyAndSdkToken_Correct) {
    std::string p = PayloadBuilder::Build(ctx, cfg, 1.0, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "\"api_key\":\"rIdDODfpGjmzllxM92dAJLhJPA\""));
    EXPECT_TRUE(Contains(p, "\"sdk_token\":\"PyZL29nBwb0gIFZ2JsqpwQlBWH8UkABf\""));
}

TEST_F(PayloadBuilderTest, Build_RequestSection_Present) {
    std::string p = PayloadBuilder::Build(ctx, cfg, 1.0, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "\"request\""));
    EXPECT_TRUE(Contains(p, "\"method\":\"POST\""));
    EXPECT_TRUE(Contains(p, "\"route_path\":\"/api/test/register\""));
    EXPECT_TRUE(Contains(p, "\"ip\":\"172.31.82.162\""));
}

TEST_F(PayloadBuilderTest, Build_ResponseSection_Present) {
    std::string p = PayloadBuilder::Build(ctx, cfg, 1112.488, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "\"response\""));
    EXPECT_TRUE(Contains(p, "\"code\":200"));
    EXPECT_TRUE(Contains(p, "\"load_time\":1112.488"));
}

TEST_F(PayloadBuilderTest, Build_MaskedKeyword_ValueRedacted) {
    cfg.maskedKeywords = {"maskedField"};
    std::string p = PayloadBuilder::Build(ctx, cfg, 1.0, "Microsoft-IIS/10.0");
    // Key must appear but the literal value "secret" must be gone
    EXPECT_TRUE(Contains(p, "\"maskedField\""));
    EXPECT_FALSE(Contains(p, "\"secret\""));
}

TEST_F(PayloadBuilderTest, Build_InternalIdAndName_Present) {
    std::string p = PayloadBuilder::Build(ctx, cfg, 1.0, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "7d8e120a-9ea0-ea78-dc27-b6dd61c13667"));
    EXPECT_TRUE(Contains(p, "Testing API - IIS"));
}

TEST_F(PayloadBuilderTest, Build_ErrorsArray_AlwaysEmpty) {
    std::string p = PayloadBuilder::Build(ctx, cfg, 1.0, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "\"errors\":[]"));
}

TEST_F(PayloadBuilderTest, Build_ServerSection_Present) {
    std::string p = PayloadBuilder::Build(ctx, cfg, 1.0, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "\"server\""));
    EXPECT_TRUE(Contains(p, "\"timezone\":\"UTC\""));
    EXPECT_TRUE(Contains(p, "\"os\""));
}

TEST_F(PayloadBuilderTest, Build_LanguageSection_Present) {
    std::string p = PayloadBuilder::Build(ctx, cfg, 1.0, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "\"language\""));
    EXPECT_TRUE(Contains(p, "\"name\":\"iis\""));
}

TEST_F(PayloadBuilderTest, Build_TruncatedRequestBody_ErrorObject) {
    ctx.requestBodyTruncated = true;
    std::string p = PayloadBuilder::Build(ctx, cfg, 1.0, "Microsoft-IIS/10.0");
    EXPECT_TRUE(Contains(p, "treblle_error"));
    EXPECT_TRUE(Contains(p, "2MB"));
}
