#include <gtest/gtest.h>
#include "Config.h"
#include <string>
#include <algorithm>
#include <windows.h>

// ── Fixture ───────────────────────────────────────────────────────────────────

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        tempDir_    = tmp;
        configPath_ = tempDir_ + L"treblle.config";
        dllPath_    = tempDir_ + L"TreblleAgentTest.dll"; // Load() strips the filename
    }

    void TearDown() override {
        DeleteFileW(configPath_.c_str());
    }

    void WriteConfig(const std::string& json) {
        HANDLE h = CreateFileW(configPath_.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, json.c_str(), static_cast<DWORD>(json.size()), &written, nullptr);
        CloseHandle(h);
    }

    std::wstring tempDir_;
    std::wstring configPath_;
    std::wstring dllPath_;
};

// ── Load / Get ────────────────────────────────────────────────────────────────

TEST_F(ConfigTest, Load_AllFields_Parsed) {
    WriteConfig(R"({
        "api_key":    "rIdDODfpGjmzllxM92dAJLhJPA",
        "sdk_token":  "PyZL29nBwb0gIFZ2JsqpwQlBWH8UkABf",
        "treblle_url": "https://custom.treblle.com",
        "debug": true
    })");

    ASSERT_TRUE(Config::Instance().Load(dllPath_));
    auto cfg = Config::Instance().Get();

    EXPECT_EQ(cfg->apiKey,     "rIdDODfpGjmzllxM92dAJLhJPA");
    EXPECT_EQ(cfg->sdkToken,   "PyZL29nBwb0gIFZ2JsqpwQlBWH8UkABf");
    EXPECT_EQ(cfg->treblleUrl, "https://custom.treblle.com");
    EXPECT_TRUE(cfg->debugMode);
    EXPECT_TRUE(cfg->loaded);
}

TEST_F(ConfigTest, Load_MissingTreblleUrl_UsesDefault) {
    WriteConfig(R"({"api_key":"k","sdk_token":"t"})");
    Config::Instance().Load(dllPath_);
    EXPECT_EQ(Config::Instance().Get()->treblleUrl, "https://ingress.treblle.com");
}

TEST_F(ConfigTest, Load_DebugFalse_Default) {
    WriteConfig(R"({"api_key":"k","sdk_token":"t"})");
    Config::Instance().Load(dllPath_);
    EXPECT_FALSE(Config::Instance().Get()->debugMode);
}

TEST_F(ConfigTest, Load_ExcludeRoutes_Parsed) {
    WriteConfig(R"({
        "api_key": "k",
        "sdk_token": "t",
        "exclude_routes": [
            {"host": "internal.example.com"},
            {"host": "api.example.com", "path": "/health"}
        ]
    })");
    Config::Instance().Load(dllPath_);
    auto cfg = Config::Instance().Get();

    ASSERT_EQ(cfg->excludeRoutes.size(), 2u);
    EXPECT_EQ(cfg->excludeRoutes[0].host, "internal.example.com");
    EXPECT_EQ(cfg->excludeRoutes[0].path, "");
    EXPECT_EQ(cfg->excludeRoutes[1].host, "api.example.com");
    EXPECT_EQ(cfg->excludeRoutes[1].path, "/health");
}

TEST_F(ConfigTest, Load_MaskedKeywordsArray_Parsed) {
    WriteConfig(R"({
        "api_key": "k",
        "sdk_token": "t",
        "masked_keywords": ["password", "ssn", "maskedField"]
    })");
    Config::Instance().Load(dllPath_);
    auto kws = Config::Instance().Get()->maskedKeywords;

    ASSERT_EQ(kws.size(), 3u);
    EXPECT_EQ(kws[0], "password");
    EXPECT_EQ(kws[1], "ssn");
    EXPECT_EQ(kws[2], "maskedField");
}

TEST_F(ConfigTest, Load_MissingMaskedKeywords_UsesDefaults) {
    WriteConfig(R"({"api_key":"k","sdk_token":"t"})");
    Config::Instance().Load(dllPath_);
    auto kws = Config::Instance().Get()->maskedKeywords;

    EXPECT_FALSE(kws.empty());
    auto it = std::find(kws.begin(), kws.end(), "password");
    EXPECT_NE(it, kws.end()) << "Default keywords must include 'password'";
}

TEST_F(ConfigTest, Load_EmptyApiKey_Fails) {
    WriteConfig(R"({"api_key":"","sdk_token":"t"})");
    EXPECT_FALSE(Config::Instance().Load(dllPath_));
}

TEST_F(ConfigTest, Load_EmptySdkToken_Fails) {
    WriteConfig(R"({"api_key":"k","sdk_token":""})");
    EXPECT_FALSE(Config::Instance().Load(dllPath_));
}

TEST_F(ConfigTest, Load_InvalidJson_Fails) {
    WriteConfig("{not valid json}");
    EXPECT_FALSE(Config::Instance().Load(dllPath_));
}

// ── IsExcluded ────────────────────────────────────────────────────────────────

TEST_F(ConfigTest, IsExcluded_HostOnly_AllPathsExcluded) {
    WriteConfig(R"({
        "api_key": "k", "sdk_token": "t",
        "exclude_routes": [{"host": "internal.example.com"}]
    })");
    Config::Instance().Load(dllPath_);

    EXPECT_TRUE(Config::Instance().IsExcluded("internal.example.com", "/"));
    EXPECT_TRUE(Config::Instance().IsExcluded("internal.example.com", "/api/users"));
    EXPECT_FALSE(Config::Instance().IsExcluded("api.example.com", "/api/users"));
}

TEST_F(ConfigTest, IsExcluded_HostAndPath_PrefixMatch) {
    WriteConfig(R"({
        "api_key": "k", "sdk_token": "t",
        "exclude_routes": [{"host": "api.example.com", "path": "/health"}]
    })");
    Config::Instance().Load(dllPath_);

    EXPECT_TRUE(Config::Instance().IsExcluded("api.example.com", "/health"));
    EXPECT_TRUE(Config::Instance().IsExcluded("api.example.com", "/health/check"));
    EXPECT_FALSE(Config::Instance().IsExcluded("api.example.com", "/api/users"));
}

TEST_F(ConfigTest, IsExcluded_CaseInsensitive) {
    WriteConfig(R"({
        "api_key": "k", "sdk_token": "t",
        "exclude_routes": [{"host": "INTERNAL.EXAMPLE.COM"}]
    })");
    Config::Instance().Load(dllPath_);

    EXPECT_TRUE(Config::Instance().IsExcluded("internal.example.com", "/path"));
    EXPECT_TRUE(Config::Instance().IsExcluded("INTERNAL.EXAMPLE.COM", "/path"));
    EXPECT_TRUE(Config::Instance().IsExcluded("Internal.Example.Com", "/path"));
}

TEST_F(ConfigTest, IsExcluded_EmptyRoutes_NeverExcluded) {
    WriteConfig(R"({"api_key":"k","sdk_token":"t","exclude_routes":[]})");
    Config::Instance().Load(dllPath_);
    EXPECT_FALSE(Config::Instance().IsExcluded("anything.com", "/path"));
}
