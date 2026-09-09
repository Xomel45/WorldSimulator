#!/usr/bin/env python3
"""Оценка потолка населения WorldSimulator на разном железе.

Методология:
  - CPU-бюджет: мозги существ (нейросети) и пространственная сетка сенсоров
    embarrassingly-parallel по агентам -> масштабируем по multi-thread
    PassMark CPU Mark относительно baseline (мой ПК: Ryzen 7 7700).
  - GPU-бюджет: рендер = инстансинг существ + террейн + тени/апскейлеры ->
    масштабируем по FP32 TFLOPS относительно baseline (RX 6650 XT).
  - Потолок населения системы = min(CPU-потолок, GPU-потолок) -- считаем по
    тому ресурсу, который упрётся первым.
  - BASELINE_POPULATION -- design target, НЕ измеренное число: у нас ещё нет
    ни одного профилированного прогона симуляции существ (движок сейчас --
    только окно + плоскость + камера). Как только появится реальный прототип
    с профилированием, эту константу и сами коэффициенты стоит пересчитать
    по факту, а не по FLOP-прикидкам.

Числа CPU/GPU ниже собраны из веб-поиска (PassMark cpubenchmark.net,
gpu-monkey/gpuspecs/cpu-monkey/videocardz) -- см. комментарии у каждой
записи. Значения без источника прямо помечены как "оценка".
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass

BASELINE_POPULATION = 5000  # design target для Ryzen 7 7700 + RX 6650 XT @ 60 FPS -- НЕ измерено

# PassMark CPU Mark (multi-thread) и single-thread score.
# Источник: cpubenchmark.net (см. отчёт исследования в чате).
CPU_BENCH = {
    "AMD Ryzen 7 7700":        {"multi": 34321, "single": 4050},
    "AMD Ryzen 5 5500":        {"multi": 19250, "single": 3061},
    "Intel Pentium N3700":     {"multi": 1256,  "single": 632},
    "Intel Core I5-12400f":    {"multi": 19547, "single": 3485},
    "AMD Ryzen 7 7730U":       {"multi": 16962, "single": 2915},
    "Intel Core i5-3470":      {"multi": 4655,  "single": 1930},
}

# FP32 TFLOPS, VRAM (ГБ), поддержка VK_KHR_ray_query (RT-тени в движке --
# graceful fallback уже есть, но без RT-железа тени просто выключены).
GPU_BENCH = {
    "AMD Radeon RX 6650 XT": {"tflops": 10.79, "vram_gb": 8, "rt": True},
    "AMD Radeon RX 580":     {"tflops": 6.175, "vram_gb": 8, "rt": False},   # GCN4, ray query нет
    "RTX 3060 (8gb)":        {"tflops": 12.85, "vram_gb": 8, "rt": True},    # 8GB-версия: 128-bit шина, на ~17% медленнее 12GB в реальных играх
}

# "встройка" в JSON не называет модель -- резолвим по owner (знаем их CPU).
INTEGRATED_GPU_BY_OWNER = {
    "Игрок 1":   {"name": "Intel HD Graphics (Braswell)",  "tflops": 0.09, "vram_gb": 0, "rt": False},
    "Игрок 2":  {"name": "AMD Radeon Vega 8 (7730U)",      "tflops": 2.05, "vram_gb": 0, "rt": False},   # GCN/Vega, не RDNA -- НЕ 7735U/680M, легко перепутать
    "Игрок 3": {"name": "Intel HD Graphics 2500",         "tflops": 0.3,  "vram_gb": 0, "rt": False},   # оценка -- точных TFLOPS не нашли, Ivy Bridge Gen7, заведомо слабее Vega 8
}

RAM_WARNING_THRESHOLD_GB = 4  # ниже этого -- предупреждаем про базовый footprint ОС+движка, не про население


@dataclass
class SystemResult:
    owner: str
    cpu_ratio: float
    gpu_ratio: float
    population_cpu: int
    population_gpu: int
    ceiling: int
    bottleneck: str
    rt_capable: bool
    ram_gb: float
    ram_warning: bool
    gpu_label: str


def resolve_gpu(system: dict) -> dict:
    gpu_name = system["gpu"]
    if gpu_name == "встройка":
        entry = INTEGRATED_GPU_BY_OWNER.get(system["owner"])
        if entry is None:
            raise KeyError(f"Не знаю встройку для {system['owner']} -- добавь в INTEGRATED_GPU_BY_OWNER")
        return entry
    entry = GPU_BENCH.get(gpu_name)
    if entry is None:
        raise KeyError(f"Нет бенчмарк-данных для GPU {gpu_name!r} -- добавь в GPU_BENCH")
    return {"name": gpu_name, **entry}


def compute(system: dict, baseline_cpu: dict, baseline_gpu: dict) -> SystemResult:
    cpu = CPU_BENCH.get(system["cpu"])
    if cpu is None:
        raise KeyError(f"Нет бенчмарк-данных для CPU {system['cpu']!r} -- добавь в CPU_BENCH")
    gpu = resolve_gpu(system)

    cpu_ratio = cpu["multi"] / baseline_cpu["multi"]
    gpu_ratio = gpu["tflops"] / baseline_gpu["tflops"]

    population_cpu = round(BASELINE_POPULATION * cpu_ratio)
    population_gpu = round(BASELINE_POPULATION * gpu_ratio)
    ceiling = min(population_cpu, population_gpu)
    bottleneck = "CPU" if population_cpu <= population_gpu else "GPU"

    ram_gb = float(system.get("ram_gb", 0))
    return SystemResult(
        owner=system["owner"],
        cpu_ratio=cpu_ratio,
        gpu_ratio=gpu_ratio,
        population_cpu=population_cpu,
        population_gpu=population_gpu,
        ceiling=ceiling,
        bottleneck=bottleneck,
        rt_capable=gpu["rt"],
        ram_gb=ram_gb,
        ram_warning=ram_gb < RAM_WARNING_THRESHOLD_GB,
        gpu_label=gpu["name"],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("json_path", nargs="?", default="dev-moments/players.json")
    args = parser.parse_args()

    with open(args.json_path, encoding="utf-8") as f:
        data = json.load(f)

    systems = data["systems"]
    baseline = next(s for s in systems if s["owner"] == "я")
    baseline_cpu = CPU_BENCH[baseline["cpu"]]
    baseline_gpu = GPU_BENCH[baseline["gpu"]]

    print(f"Baseline: {baseline['owner']} -- {baseline['cpu']} + {baseline['gpu']}")
    print(f"Design target на baseline: {BASELINE_POPULATION} существ @ 60 FPS (не измерено, см. docstring)\n")

    results = [compute(s, baseline_cpu, baseline_gpu) for s in systems]
    results.sort(key=lambda r: r.ceiling, reverse=True)

    name_w = max(len(r.owner) for r in results)
    for r in results:
        print(f"{r.owner:<{name_w}}  потолок ~{r.ceiling:>5} существ  "
              f"(CPU:{r.population_cpu:>5} / GPU:{r.population_gpu:>5}, узкое место: {r.bottleneck})  "
              f"RT-тени: {'да' if r.rt_capable else 'нет':<3}  GPU: {r.gpu_label}")
        if r.ram_warning:
            print(f"{'':<{name_w}}  [!] {r.ram_gb:.0f}ГБ RAM -- под вопросом сам запуск (ОС+драйверы+движок), "
                  f"не только население")

    return 0


if __name__ == "__main__":
    sys.exit(main())
