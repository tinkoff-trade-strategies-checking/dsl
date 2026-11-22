# dsl

C++20 библиотека языка стратегий DSL проекта «Т-Инвестиции: тренажёр и лаборатория стратегий» (лексер, парсер, линтер, статические проверки, интерпретатор, детектор lookahead). Подключается к ядру `core`.

Спецификация языка — `../documentation/dsl.md`. Общие знания — `../documentation/README.md`. Процесс работы и указатели на остальные репозитории — `../home/workflow.md` и `../home/AGENTS.md`.

Если документация репозитория противоречит существующему коду, следуй соглашениям, принятым в самом коде, а в финальном ответе укажи каждое найденное несоответствие между документацией и кодом и предложи исправления. Исключение — инварианты из `../documentation/invariants.md`: их нарушение в коде считается багом, его не повторяют, а сообщают о нём.

## Стек и связи
- C++20, CMake (≥ 3.25), Ninja, vcpkg / apt.
- Зависимости библиотеки — только стандартная библиотека C++20; для тестов используются GoogleTest и RapidCheck.
- Используется ядром `core` для проверки и исполнения торговых стратегий (`CheckStrategy`, прогон в симуляции, разбор расхождений).

## Как работать над задачей
Полностью — `../home/workflow.md`. Коротко:
1. Работа идёт по issue с доски https://github.com/orgs/tinkoff-trade-strategies-checking/projects/2. Перед стартом прочитай issue вместе с комментариями: там ход работы и отвергнутые варианты.
2. Прочитай `../documentation/dsl.md`, ФТ, INV и решения раздела «Архитектура» в `../documentation/decisions.md`.
3. Ветка `<тип>/<N>`, изменения вместе с тестами, PR с `Closes #N`.
4. Не успел закончить — оставь в issue комментарий: что сделано, что осталось, что пробовали и почему отказались.
5. Изменение сделало документы неверными — обнови их в том же PR (этот файл, `../documentation/dsl.md`, `../documentation/status.md`, `../documentation/decisions.md`).

## Критично
Ключевые инварианты (`../documentation/invariants.md`) и ограничения (`../documentation/dsl.md`):
- **INV-1.** Без подглядывания в будущее: интерпретатор видит только бары `≤` текущего шага `t` (плюс прогрев до шага `0`), статический и динамический детектор lookahead.
- **INV-4.** Детерминизм: сборка строго с `-ffp-contract=off` (без FMA), запрет `-ffast-math`, `-Ofast`, `-funsafe-math-optimizations`, `/fp:fast` (проверяется в `CMakeLists.txt` и тестом `NoFusedMultiplyAdd` в Debug и Release), никаких системных часов, случайности без seed и порядка обхода hash-контейнеров.
- **INV-6.** Выражения DSL и расчётные цены заявок (`limit`, `stop`, `take_profit`) считаются в IEEE 754 `double` (`-ffp-contract=off`), но денежные расчёты и детерминированное округление к шагу цены (`HALF_UP`) в `core::Decimal` остаются на стороне `core`.
- **INV-9.** Алгоритмы не видят новостей и внешних данных.
- Ограничения безопасности и бюджета выполнения (НФТ-7): текст до 64 КиБ, до 100 правил, до 50 параметров, глубина выражения до 100, до 2000 узлов AST, до 256 узлов DAG индикаторов, смещение и период до 5000 баров; поддержка кооперативной отмены.

## Стандарты
Стиль C++ — LLVM, решение владельца продукта: `../home/standards.md`, раздел «Стиль C++». Конфиги — `.clang-format` (`BasedOnStyle: LLVM`) и `.clang-tidy` (`readability-identifier-naming`, проверяет `include/`, `src/`, `tests/`); проверяются в CI, падение — ошибка.
- Имена: типы, enum и их значения — `PascalCase`; переменные, параметры и поля (без суффикса `_`) — `PascalCase`; функции и методы — `camelCase`; константы — `k` + `PascalCase` (`constexpr`, глобальные и статические `const`); пространства имён — `lower_case`; макросы — `UPPER_CASE`; файлы — `snake_case`.
- Поле не может называться как его тип (`-Wchanges-meaning`).
- `-Wshadow` обязателен: не называй параметры конструктора или локальные переменные как поля и глобальные типы.
- Имя, обязанное совпасть с внешним API (например, GoogleTest), помечай `// NOLINT(readability-identifier-naming)` с пояснением.
- Форматируй только clang-format 18 через `scripts/dev.sh format`, имена проверяет и правит `scripts/dev.sh tidy` / `scripts/dev.sh tidy-fix` (после `tidy-fix` снова `format`).

## Структура
- `include/dsl/` — публичные заголовки библиотеки (`dsl/dsl.hpp`, `dsl/types.hpp`, `dsl/version.hpp`, `dsl/ast.hpp`, `dsl/check.hpp`).
- `src/` — реализация библиотеки `dsl` (цель `dsl::dsl`): `version.cpp`, `lexer.cpp`, `parser.cpp`, `printer.cpp`, `checker.cpp`, `linter.cpp`, `check.cpp` и внутренние заголовки `src/internal/{lexer,analysis}.hpp`.
- `tests/` — тесты на GoogleTest и RapidCheck (цель `dsl_tests`): `version_test.cpp`, `check_test.cpp`, `build/flags_test.cpp` (проверка `-ffp-contract=off`).
- `CMakeLists.txt`, `CMakePresets.json` — конфигурация CMake (пресеты `debug`, `release`, `vcpkg-debug`, `vcpkg-release`).
- `Dockerfile.dev`, `scripts/dev.sh` — Docker-окружение (`pac-dsl-dev`) для сборки, тестов и проверок стиля.
- `.github/workflows/ci.yml` — CI (`build-test` для `debug` и `release`, `style`, `pr-size`).

## Команды
На хосте может не быть компилятора с C++20 — основной путь это Docker. Сборка всегда с `-j2`.

```bash
scripts/dev.sh image         # собрать образ pac-dsl-dev (один раз и при смене Dockerfile.dev)
scripts/dev.sh build         # конфигурация и сборка (пресет debug)
scripts/dev.sh test          # сборка и ctest
scripts/dev.sh shell         # интерактивная оболочка в контейнере
scripts/dev.sh format        # clang-format по include/, src/ и tests/ (стиль LLVM)
scripts/dev.sh format-check  # проверка формата без изменений (--dry-run --Werror)
scripts/dev.sh tidy          # clang-tidy: именование; ошибка при предупреждениях
scripts/dev.sh tidy-fix      # clang-tidy --fix: автоисправление имён (потом format)
scripts/dev.sh clean         # удалить том со сборкой pac-dsl-build
PRESET=release scripts/dev.sh test   # прогон Release-сборки (-O3 -ffp-contract=off)
```

Нативно (Linux с `g++-13`, `cmake >= 3.25`, `ninja`, `libgtest-dev`, `librapidcheck-dev`):

```bash
cmake --preset debug && cmake --build --preset debug -j2 && ctest --preset debug
cmake --preset release && cmake --build --preset release -j2 && ctest --preset release
```

Через vcpkg (нужен `VCPKG_ROOT`): пресеты `vcpkg-debug` / `vcpkg-release`.

Только библиотека без тестов: `cmake --preset debug -DDSL_BUILD_TESTS=OFF`.
