#pragma once

#include "renderer/Vertex.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wsim {

// Минимальный OBJ+MTL загрузчик под наш экспорт из Blender:
// v / vn / f (треугольники), usemtl / mtllib; Kd материала → Vertex::color.
// Возвращает готовые к Mesh::create вершины и индексы (с дедупликацией).
// Бросает std::runtime_error, если файл не найден или пуст.
[[nodiscard]] std::pair<std::vector<Vertex>, std::vector<uint32_t>>
loadObj(const std::string& path);

}  // namespace wsim
