#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_dir}/.build/tests"
mkdir -p "${build_dir}"

# Everything except the translation units that need esphome headers. Kept as an
# exclusion so a new pure source file joins the suite without being listed here
# and in tests/platformio.ini.
esphome_dependent=(
  omron_ble_client.cpp
  omron_ble_client_bond.cpp
  omron_entities.cpp
)

source_paths=()
for path in "${repo_dir}"/components/omron/*.cpp; do
  name="$(basename "${path}")"
  skip=0
  for excluded in "${esphome_dependent[@]}"; do
    if [[ "${name}" == "${excluded}" ]]; then
      skip=1
      break
    fi
  done
  if [[ "${skip}" -eq 0 ]]; then
    source_paths+=("${path}")
  fi
done

# gnu++20, because that is what the firmware is built with: the ESP32 platform
# sets it in esphome/components/esp32/__init__.py. Anything that differs between
# dialects - overload resolution, aggregate initialisation, implicit this
# capture - is then tested in the language the node actually runs.
g++ -std=gnu++20 -Wall -Wextra -Werror \
  -I"${repo_dir}/components/omron" \
  -I"${repo_dir}/tests" \
  "${source_paths[@]}" \
  "${repo_dir}"/tests/test_*.cpp \
  -o "${build_dir}/test_omron_pure"

echo "compiled ${#source_paths[@]} component sources"
"${build_dir}/test_omron_pure"
