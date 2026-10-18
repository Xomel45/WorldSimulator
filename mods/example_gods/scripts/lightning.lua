-- Сила "Молния": срабатывает один раз на нажатие (в mod.json interval = -1).
--
-- Скрипт возвращает функцию apply(ctx, p):
--   p.x, p.z    -- точка под курсором
--   p.radius    -- радиус кисти (колесо с Ctrl или [ ])
--   p.dt        -- шаг времени, секунд
--   p.first     -- true на первом вызове после нажатия кнопки
-- ctx -- рука бога, см. mods/README.md.
return function(ctx, p)
  -- Убиваем всех в центре удара, а ландшафт под ударом проседает воронкой.
  ctx:sound("zap", p.x, p.z)              -- звук разряда в точке удара
  local killed = ctx:erase(p.x, p.z, p.radius * 0.6)
  ctx:raise(p.x, p.z, p.radius, -3)
  if killed > 0 then
    ctx:log("молния убила существ: " .. killed)
  end
end
