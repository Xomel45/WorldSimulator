-- Сила "Вулкан": работает, пока кнопка зажата (interval не задан = каждый шаг симуляции).
-- Возвращать можно и таблицу с полем apply -- удобно, если рядом есть вспомогательные функции.
local M = {}

local function crater_killzone(ctx, x, z, r)
  -- У самого жерла существ испепеляет.
  ctx:erase(x, z, r * 0.25)
end

function M.apply(ctx, p)
  -- Широкий конус растёт быстро, жерло в центре понижается: получается кратер.
  ctx:raise(p.x, p.z, p.radius, 14 * p.dt)
  ctx:raise(p.x, p.z, p.radius * 0.3, -9 * p.dt)
  crater_killzone(ctx, p.x, p.z, p.radius)
end

return M
