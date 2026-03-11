#include <gtest/gtest.h>
#include <core.hpp>

TEST(CoreTest, FastStartup) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1; // Минимум ресурсов для теста
    cfg.plugins.auto_load = false; // Не грузим реальные либы
    
    gn::Core core(cfg);
    core.run_async();
    EXPECT_TRUE(core.is_running());
    core.stop();
}

#include "core.h" // Твой C-заголовок

TEST(CapiTest, LifecycleSanity) {
    gn_config_t cfg{};
    cfg.config_dir = "./test_id";
    cfg.log_level = "off"; // Чтобы не мусорить в консоль при тестах
    cfg.listen_port = 0;

    // 1. Создание (проверяет gn_core_create и конструктор Core)
    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    // 2. Проверка публичного ключа (проверяет маршалинг строк)
    char buf[65];
    size_t len = gn_core_get_user_pubkey(core, buf, sizeof(buf));
    EXPECT_GT(len, 0u);
    EXPECT_EQ(strlen(buf), len);

    // 3. Жизненный цикл (проверяет потоки и стоп)
    gn_core_run_async(core, 1);
    gn_core_stop(core);

    // 4. Удаление (проверяет деструктор и Pimpl cleanup)
    gn_core_destroy(core);
}
