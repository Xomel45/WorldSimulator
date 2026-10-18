#pragma once

#include "sim/Powers.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace wsim {

// Одно Lua-состояние на мод. Скрипту доступны только безопасные библиотеки
// (base без load/dofile/loadfile, table, string без dump, math, utf8) -- ни io,
// ни os, ни package, ни debug, поэтому мод не может ни читать файлы, ни
// запускать процессы. Память ограничена, на каждый вызов дан бюджет
// инструкций (бесконечный цикл не повесит игру). Единственный способ
// вмешаться в мир -- объект ctx (GodContext), тот же, что у встроенных сил.
//
// Lua-заголовки спрятаны в .cpp (pimpl): остальной движок их не видит.
class LuaSandbox {
public:
    explicit LuaSandbox(std::string modId);
    ~LuaSandbox();
    LuaSandbox(const LuaSandbox&)            = delete;
    LuaSandbox& operator=(const LuaSandbox&) = delete;

    // Исполняет файл скрипта; он должен вернуть функцию apply(ctx, p) или
    // таблицу с полем apply. Возвращает номер силы-обработчика для call()
    // либо текст ошибки.
    struct Loaded { int handler = -1; std::string error; };
    [[nodiscard]] Loaded loadPowerScript(const std::filesystem::path& file);

    // Вызывает обработчик. Пустая строка -- всё хорошо, иначе текст ошибки
    // (после ошибки обработчик вызывать не нужно: вызывающий отключает силу).
    [[nodiscard]] std::string call(int handler, GodContext& god, const PowerCall& call);

    struct Impl;  // непрозрачно; публично только чтобы им пользовались функции в .cpp

private:
    std::unique_ptr<Impl> m_impl;
};

}  // namespace wsim
