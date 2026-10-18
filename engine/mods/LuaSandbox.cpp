#include "mods/LuaSandbox.hpp"

#include "core/Logger.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <stdexcept>
#include <vector>
#include <sstream>

namespace wsim {

namespace {
constexpr std::size_t kMemoryLimit    = 32u * 1024u * 1024u;  // на мод
constexpr long        kCallBudget     = 4'000'000;            // инструкций на вызов силы
constexpr long        kLoadBudget     = 20'000'000;           // инструкций на загрузку скрипта
constexpr int         kHookGranularity = 1000;
}  // namespace

struct LuaSandbox::Impl {
    lua_State*  L = nullptr;
    std::string modId;
    std::size_t memUsed = 0;
    long        budget = 0;
    GodContext* god = nullptr;
    int         ctxRef = LUA_NOREF;
};

namespace {

// Lua отдаёт память через этот аллокатор: считаем и не даём превысить лимит мода.
void* allocator(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* im = static_cast<LuaSandbox::Impl*>(ud);
    const std::size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        im->memUsed -= old;
        std::free(ptr);
        return nullptr;
    }
    if (im->memUsed - old + nsize > kMemoryLimit) return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p) im->memUsed = im->memUsed - old + nsize;
    return p;
}

LuaSandbox::Impl* implOf(lua_State* L) { return *static_cast<LuaSandbox::Impl**>(lua_getextraspace(L)); }

void hook(lua_State* L, lua_Debug*) {
    auto* im = implOf(L);
    im->budget -= kHookGranularity;
    if (im->budget <= 0) luaL_error(L, "скрипт превысил лимит инструкций (бесконечный цикл?)");
}

// Аргумент-число: конечное, в разумных пределах (мод не должен ронять игру NaN-ами и гигантскими числами).
float num(lua_State* L, int idx, float lo, float hi) {
    const lua_Number v = luaL_checknumber(L, idx);
    if (!std::isfinite(v)) luaL_argerror(L, idx, "число должно быть конечным");
    return std::clamp(static_cast<float>(v), lo, hi);
}

GodContext& god(lua_State* L) {
    auto* im = implOf(L);
    if (!im->god) luaL_error(L, "ctx можно использовать только внутри вызова силы");
    return *im->god;
}

constexpr float kCoord = 100000.0f;

// ctx:height(x, z) -> число
int l_height(lua_State* L) {
    lua_pushnumber(L, god(L).height(num(L, 2, -kCoord, kCoord), num(L, 3, -kCoord, kCoord)));
    return 1;
}
// ctx:raise(x, z, radius, amount)
int l_raise(lua_State* L) {
    god(L).raise(num(L, 2, -kCoord, kCoord), num(L, 3, -kCoord, kCoord), num(L, 4, 0.5f, 100.0f), num(L, 5, -100.0f, 100.0f));
    return 0;
}
// ctx:flatten(x, z, radius, targetHeight, rate)
int l_flatten(lua_State* L) {
    god(L).flatten(num(L, 2, -kCoord, kCoord), num(L, 3, -kCoord, kCoord), num(L, 4, 0.5f, 100.0f),
                   num(L, 5, -200.0f, 200.0f), num(L, 6, 0.0f, 1.0f));
    return 0;
}
// ctx:spawn(x, z [, {speed=0.5, size=1, ...}]) -> bool
int l_spawn(lua_State* L) {
    const float x = num(L, 2, -kCoord, kCoord), z = num(L, 3, -kCoord, kCoord);
    std::vector<std::pair<Genome::TraitIndex, float>> traits;
    if (lua_istable(L, 4)) {
        lua_pushnil(L);
        while (lua_next(L, 4) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isnumber(L, -1)) {
                if (const auto idx = Genome::traitFromName(lua_tostring(L, -2)))
                    traits.emplace_back(*idx, std::clamp(static_cast<float>(lua_tonumber(L, -1)), -1.0f, 1.0f));
            }
            lua_pop(L, 1);
        }
    }
    lua_pushboolean(L, god(L).spawn(x, z, traits));
    return 1;
}
// ctx:erase(x, z, radius) -> сколько убрано
int l_erase(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(god(L).erase(num(L, 2, -kCoord, kCoord), num(L, 3, -kCoord, kCoord), num(L, 4, 0.5f, 100.0f))));
    return 1;
}
// ctx:sound(name [, x, z [, volume]]) -> bool: "thud", "pop", "whoosh", "zap", "chime", "knock"
int l_sound(lua_State* L) {
    const char* name = luaL_checkstring(L, 2);
    const float x = lua_isnone(L, 3) ? 0.0f : num(L, 3, -kCoord, kCoord), z = lua_isnone(L, 4) ? 0.0f : num(L, 4, -kCoord, kCoord);
    const float vol = lua_isnone(L, 5) ? 1.0f : num(L, 5, 0.0f, 1.0f);
    lua_pushboolean(L, god(L).sound(name, x, z, vol));
    return 1;
}
// ctx:build(x, z [, yaw]) -> bool: дом на суше, площадка выравнивается
int l_build(lua_State* L) {
    lua_pushboolean(L, god(L).build(num(L, 2, -kCoord, kCoord), num(L, 3, -kCoord, kCoord), lua_isnone(L, 4) ? 0.0f : num(L, 4, -3600.0f, 3600.0f)));
    return 1;
}
// ctx:erase_buildings(x, z, radius) -> сколько убрано
int l_erase_buildings(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(god(L).eraseBuildings(num(L, 2, -kCoord, kCoord), num(L, 3, -kCoord, kCoord), num(L, 4, 0.5f, 100.0f))));
    return 1;
}
// ctx:boulder(x, z, radius) -> bool: валун падает с высоты, катится, давит существ
int l_boulder(lua_State* L) {
    lua_pushboolean(L, god(L).boulder(num(L, 2, -kCoord, kCoord), num(L, 3, -kCoord, kCoord), num(L, 4, 0.5f, 6.0f)));
    return 1;
}
// ctx:count(x, z, radius) -> сколько существ в круге
int l_count(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(god(L).countNear(num(L, 2, -kCoord, kCoord), num(L, 3, -kCoord, kCoord), num(L, 4, 0.5f, 100.0f))));
    return 1;
}
// ctx:log(text) и глобальный print(...) -- в лог игры с пометкой мода
int l_log(lua_State* L) {
    const int first = lua_istable(L, 1) ? 2 : 1;  // ctx:log(...) или print(...)
    std::ostringstream out;
    for (int i = first; i <= lua_gettop(L); ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        out << (i > first ? " " : "") << std::string_view(s, std::min<size_t>(len, 500));
        lua_pop(L, 1);
    }
    Logger::info("[mod {}] {}", implOf(L)->modId, out.str());
    return 0;
}

void openSafeLibs(lua_State* L) {
    const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (const auto& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // Всё, что умеет читать файлы или выполнять строки как код, убираем.
    for (const char* name : {"dofile", "loadfile", "load", "loadstring", "require", "collectgarbage"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "print");
}

void buildCtx(LuaSandbox::Impl* im) {
    lua_State* L = im->L;
    lua_newtable(L);
    const luaL_Reg fns[] = {{"height", l_height}, {"raise", l_raise}, {"flatten", l_flatten}, {"spawn", l_spawn},
                            {"erase", l_erase},   {"boulder", l_boulder}, {"build", l_build}, {"sound", l_sound}, {"erase_buildings", l_erase_buildings}, {"count", l_count}, {"log", l_log},         {nullptr, nullptr}};
    luaL_setfuncs(L, fns, 0);
    im->ctxRef = luaL_ref(L, LUA_REGISTRYINDEX);
}

}  // namespace

LuaSandbox::LuaSandbox(std::string modId) : m_impl(std::make_unique<Impl>()) {
    m_impl->modId = std::move(modId);
#if LUA_VERSION_NUM >= 505
    m_impl->L = lua_newstate(allocator, m_impl.get(), static_cast<unsigned>(std::random_device{}()));
#else
    m_impl->L = lua_newstate(allocator, m_impl.get());
#endif
    if (!m_impl->L) throw std::runtime_error("Lua: не удалось создать состояние");
    *static_cast<Impl**>(lua_getextraspace(m_impl->L)) = m_impl.get();
    openSafeLibs(m_impl->L);
    buildCtx(m_impl.get());
    lua_sethook(m_impl->L, hook, LUA_MASKCOUNT, kHookGranularity);
}

LuaSandbox::~LuaSandbox() {
    if (m_impl && m_impl->L) lua_close(m_impl->L);
}

LuaSandbox::Loaded LuaSandbox::loadPowerScript(const std::filesystem::path& file) {
    Impl*      im = m_impl.get();
    lua_State* L  = im->L;

    std::ifstream in(file, std::ios::binary);
    if (!in) return {-1, "не удалось открыть " + file.string()};
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string src = buf.str();
    if (src.size() > 512 * 1024) return {-1, "скрипт слишком большой: " + file.filename().string()};

    const std::string chunk = "@" + file.filename().string();
    if (luaL_loadbufferx(L, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK) {  // "t" -- только текст, без байткода
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return {-1, err};
    }
    im->budget = kLoadBudget;
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_pop(L, 1);
        return {-1, err};
    }
    // Результат: функция или таблица с apply.
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "apply");
        lua_remove(L, -2);
    }
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return {-1, file.filename().string() + ": скрипт должен вернуть функцию apply(ctx, p) или таблицу с полем apply"};
    }
    return {luaL_ref(L, LUA_REGISTRYINDEX), {}};
}

std::string LuaSandbox::call(int handler, GodContext& godCtx, const PowerCall& c) {
    Impl*      im = m_impl.get();
    lua_State* L  = im->L;

    lua_rawgeti(L, LUA_REGISTRYINDEX, handler);
    lua_rawgeti(L, LUA_REGISTRYINDEX, im->ctxRef);
    lua_createtable(L, 0, 5);
    lua_pushnumber(L, c.x);      lua_setfield(L, -2, "x");
    lua_pushnumber(L, c.z);      lua_setfield(L, -2, "z");
    lua_pushnumber(L, c.radius); lua_setfield(L, -2, "radius");
    lua_pushnumber(L, c.dt);     lua_setfield(L, -2, "dt");
    lua_pushboolean(L, c.first); lua_setfield(L, -2, "first");

    im->budget = kCallBudget;
    im->god    = &godCtx;
    const int rc = lua_pcall(L, 2, 0, 0);
    im->god      = nullptr;
    if (rc == LUA_OK) return {};
    std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "неизвестная ошибка";
    lua_pop(L, 1);
    return err;
}

}  // namespace wsim
