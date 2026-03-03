#include <gtest/gtest.h>
#include "config.hpp"
#include <filesystem>

namespace fs = std::filesystem;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Можно добавить инициализацию, если нужна общая среда для тестов
    }
};

// 1. Проверка дефолтных значений
TEST_F(ConfigTest, LoadDefaults) {
    Config cfg{true}; 
    // Мы ожидаем, что после создания объекта дефолты уже там
    EXPECT_EQ(cfg.get_or<int>("core.listen_port", 0), 25565);
    EXPECT_EQ(cfg.get_or<std::string>("logging.level", ""), "info");
    EXPECT_TRUE(cfg.get_or<bool>("plugins.auto_load", false));
}

// 2. Проверка ручной установки и получения значений (разные типы)
TEST_F(ConfigTest, SetAndGetValues) {
    Config cfg{true};
    
    cfg.set("test.int", 42);
    cfg.set("test.bool", true);
    cfg.set("test.string", std::string("hello"));
    cfg.set("test.path", fs::path("/tmp/test"));

    EXPECT_EQ(cfg.get<int>("test.int").value(), 42);
    EXPECT_TRUE(cfg.get<bool>("test.bool").value());
    EXPECT_EQ(cfg.get<std::string>("test.string").value(), "hello");
    EXPECT_EQ(cfg.get<fs::path>("test.path").value(), fs::path("/tmp/test"));
}

// 3. Проверка парсинга JSON (самый важный тест для конфига)
TEST_F(ConfigTest, ParseJsonString) {
    Config cfg{true};
    std::string json = R"({
        "core": {
            "listen_port": 8080,
            "max_connections": 500
        },
        "custom": "value",
        "path_as_string": "/etc/goodnet"
    })";

    ASSERT_TRUE(cfg.load_from_string(json));
    
    EXPECT_EQ(cfg.get_or<int>("core.listen_port", 0), 8080);
    EXPECT_EQ(cfg.get_or<int>("core.max_connections", 0), 500);
    EXPECT_EQ(cfg.get_or<std::string>("custom", ""), "value");
    
    // Проверка твоей логики: строка из JSON должна уметь доставаться как fs::path
    auto p = cfg.get<fs::path>("path_as_string");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p.value(), fs::path("/etc/goodnet"));
}

// 4. Проверка обработки отсутствующих ключей
TEST_F(ConfigTest, MissingKeys) {
    Config cfg{true};
    EXPECT_FALSE(cfg.has("non_existent_key"));
    EXPECT_EQ(cfg.get<int>("non_existent_key"), std::nullopt);
    EXPECT_EQ(cfg.get_or<int>("non_existent_key", 999), 999);
}

// 5. Проверка несоответствия типов (Type Mismatch)
TEST_F(ConfigTest, TypeMismatch) {
    Config cfg{true};
    cfg.set("test.number", 123);
    
    // Пытаемся достать число как строку — должно вернуть nullopt и залогировать ошибку
    auto val = cfg.get<std::string>("test.number");
    EXPECT_EQ(val, std::nullopt);
}