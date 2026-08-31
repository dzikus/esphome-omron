#!/usr/bin/env bash
set -euo pipefail

# ESP-IDF passes -Wall -Wextra -Werror and then cancels it with a trailing
# -Wno-error, so a warning from the component is emitted and the build still
# succeeds. This turns those warnings back into a failure.

config="${1:?usage: $0 <esphome-config.yaml> [board]}"
board="${2:-esp32dev}"
esphome_bin="${ESPHOME:-esphome}"
# Outside the repo, out of reach of git add.
log="$(mktemp -t esp32-warnings-XXXXXX.log)"

# esphome copies a source into the build tree only when its content changed, so
# touching it does not force a rebuild. Drop the objects instead.
config_dir="$(cd "$(dirname "${config}")" && pwd)"
find "${config_dir}/.esphome/build" -path "*/components/omron/*" -name "*.obj" -delete 2>/dev/null || true

"${esphome_bin}" -s board "${board}" compile "${config}" > "${log}" 2>&1 || {
  echo "build failed; see ${log}" >&2
  tail -30 "${log}" >&2
  exit 1
}

lines="$(mktemp -t esp32-warnings-lines-XXXXXX.log)"
tr '\r' '\n' < "${log}" > "${lines}"

sources=("$(dirname "${BASH_SOURCE[0]}")/../components/omron"/*.cpp)
built="$(grep -oE "Building CXX object [^ ]*components/omron/[a-z_]+\.cpp" "${lines}" | sort -u | wc -l)"
if [[ "${built}" -ne "${#sources[@]}" ]]; then
  echo "${built} of ${#sources[@]} component translation units were compiled;" >&2
  echo "the verdict would cover only part of the component" >&2
  exit 1
fi

if grep -E "components/omron/.*(warning|error):" "${lines}"; then
  echo "component warnings above; see ${log}" >&2
  exit 1
fi
echo "no component warnings (${built} translation units compiled)"
