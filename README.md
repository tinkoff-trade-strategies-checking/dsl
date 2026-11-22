# dsl

C++20 библиотека языка стратегий DSL для проекта «Т-Инвестиции: тренажёр и лаборатория стратегий».

## Структура
- `include/dsl/` — публичные заголовки библиотеки (`dsl::dsl`).
- `src/` — исходный код библиотеки.
- `tests/` — модульные тесты (GoogleTest) и тесты свойств (RapidCheck).
- `scripts/dev.sh`, `Dockerfile.dev` — сборка, тесты и проверка стиля в Docker (`pac-dsl-dev`).

## Сборка и тесты

В Docker (рекомендуемый путь):

```bash
scripts/dev.sh image   # собрать образ pac-dsl-dev
scripts/dev.sh test    # собрать и прогнать тесты (PRESET=debug или release)
scripts/dev.sh format-check
scripts/dev.sh tidy
```

Нативно (Ubuntu 24.04 с `g++-13`, `cmake`, `ninja-build`, `libgtest-dev`, `librapidcheck-dev`):

```bash
cmake --preset debug && cmake --build --preset debug -j2 && ctest --preset debug
```

## Документация и процесс
- Спецификация языка DSL: [../documentation/dsl.md](../documentation/dsl.md)
- Процесс разработки и правила PR: [../home/workflow.md](../home/workflow.md)
- Инструкции для агентов: [AGENTS.md](AGENTS.md)
