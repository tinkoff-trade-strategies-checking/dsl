#!/usr/bin/env bash
# Сборка и тесты библиотеки DSL внутри Docker (на хосте может не быть C++20-компилятора).
#
#   scripts/dev.sh image         собрать образ pac-dsl-dev (один раз и при смене зависимостей)
#   scripts/dev.sh build         конфигурация и сборка
#   scripts/dev.sh test          сборка и ctest
#   scripts/dev.sh all           image + test (по умолчанию)
#   scripts/dev.sh shell         интерактивная оболочка в контейнере
#   scripts/dev.sh format        переформатировать include/, src/ и tests/ (clang-format, стиль LLVM)
#   scripts/dev.sh format-check  проверить формат без изменений (--dry-run --Werror)
#   scripts/dev.sh tidy          clang-tidy: именование (readability-identifier-naming)
#   scripts/dev.sh tidy-fix      clang-tidy --fix: исправить именование автоматически
#   scripts/dev.sh clean         удалить том со сборкой
#
# Переменные: PRESET (debug), JOBS (2), IMAGE (pac-dsl-dev), BUILD_VOLUME (pac-dsl-build).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

IMAGE="${IMAGE:-pac-dsl-dev}"
PRESET="${PRESET:-debug}"
JOBS="${JOBS:-2}"
BUILD_VOLUME="${BUILD_VOLUME:-pac-dsl-build}"

# Git Bash на Windows: не переписывать пути контейнера, а путь хоста взять в виде C:/...
export MSYS_NO_PATHCONV=1
if SRC_DIR="$(pwd -W 2>/dev/null)"; then
  CHOWN_BACK=""
else
  SRC_DIR="$(pwd)"
  # Linux/macOS: контейнер работает от root; после правок вернуть файлы владельцу, а не
  # оставлять root-овыми (build — именованный том, его владелец не важен).
  CHOWN_BACK="; chown -R $(id -u):$(id -g) include src tests"
fi

# Каталог build — в именованном томе: быстрее, чем bind mount, и не засоряет рабочую копию.
# Использование: docker_run [опции docker] -- команда...
docker_run() {
  local opts=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do opts+=("$1"); shift; done
  shift
  docker run --rm "${opts[@]}" \
    -v "${SRC_DIR}:/src" \
    -v "${BUILD_VOLUME}:/src/build" \
    -w /src \
    "${IMAGE}" "$@"
}

cmd_image() {
  docker build -f Dockerfile.dev -t "${IMAGE}" .
}

cmd_build() {
  docker_run -- bash -euc "cmake --preset ${PRESET} && cmake --build --preset ${PRESET} -j${JOBS}"
}

cmd_test() {
  docker_run -- bash -euc "cmake --preset ${PRESET} && cmake --build --preset ${PRESET} -j${JOBS} && ctest --preset ${PRESET} -j${JOBS}"
}

# Исходники C++ под проверку стиля (сгенерированный код в build/ не входит).
FORMAT_FILES='find include src tests -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.cc" \) -print0 | sort -z'

cmd_format() {
  docker_run -- bash -euc "${FORMAT_FILES} | xargs -0 clang-format -i${CHOWN_BACK}"
}

cmd_format_check() {
  docker_run -- bash -euc "${FORMAT_FILES} | xargs -0 clang-format --dry-run --Werror"
}

# clang-tidy читает compile_commands.json пресета.
cmd_tidy_run() {
  local extra="$1"
  docker_run -- bash -euc "
    cmake --preset ${PRESET} >/dev/null
    run-clang-tidy -p build/${PRESET} -j ${JOBS} -quiet ${extra} '^/src/(include|src|tests)/'
    ${CHOWN_BACK_FIX:-}
  "
}

cmd_tidy() {
  cmd_tidy_run ""
}

cmd_tidy_fix() {
  CHOWN_BACK_FIX="${CHOWN_BACK#; }"
  cmd_tidy_run "-fix -clang-apply-replacements-binary /usr/bin/clang-apply-replacements-18"
}

cmd_shell() {
  docker_run -it -- bash
}

cmd_clean() {
  docker volume rm "${BUILD_VOLUME}"
}

case "${1:-all}" in
  image) cmd_image ;;
  build) cmd_build ;;
  test) cmd_test ;;
  format) cmd_format ;;
  format-check) cmd_format_check ;;
  tidy) cmd_tidy ;;
  tidy-fix) cmd_tidy_fix ;;
  shell) cmd_shell ;;
  clean) cmd_clean ;;
  all) cmd_image && cmd_test ;;
  *) sed -n '2,16p' "$0"; exit 2 ;;
esac
