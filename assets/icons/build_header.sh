#!/usr/bin/env bash
# Stitch all 96×48 RGB565 snippets into firmware/main/pageros_app_icons.h.
# Run after downscale.py has produced rgb565/*_96x48_lanczos.h.snippet.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../firmware/main/pageros_app_icons.h"

USER_APPS=(news email maps dm drive ssh askai stocks wiki translate)
BUILTINS=(settings store notif quick notes sysmon identity nfc)

{
  echo "// SPDX-License-Identifier: Apache-2.0"
  echo "// AUTO-GENERATED — do not edit by hand."
  echo "// Regenerate: assets/icons/build_header.sh"
  echo "#pragma once"
  echo "#include <stdint.h>"
  echo "#include <string.h>"
  echo "#define PAGEROS_APP_ICON_W 96"
  echo "#define PAGEROS_APP_ICON_H 48"
  echo "#define PAGEROS_APP_ICON_PIXELS (PAGEROS_APP_ICON_W*PAGEROS_APP_ICON_H)"
  echo

  for n in "${USER_APPS[@]}"; do
    cat "$HERE/rgb565/${n}_96x48_lanczos.h.snippet" \
      | sed "s/icon_${n}_96x48_lanczos/icon_${n}/"
    echo
  done

  for n in "${BUILTINS[@]}"; do
    cat "$HERE/rgb565/builtin_${n}_96x48_lanczos.h.snippet" \
      | sed "s/icon_builtin_${n}_96x48_lanczos/icon_builtin_${n}/"
    echo
  done

  cat <<'EOF'
// Lookup by full reverse-DNS app id (sideloaded / store-installed apps).
static inline const uint16_t *pageros_app_icon_for(const char *app_id) {
    if (!app_id) return NULL;
EOF
  for n in "${USER_APPS[@]}"; do
    printf '    if (strcmp(app_id, "%s.pageros.app") == 0) return icon_%s;\n' "$n" "$n"
  done
  cat <<'EOF'
    return NULL;
}

// Lookup by built-in tile href (e.g. "shell:settings", "builtin:store").
static inline const uint16_t *pageros_builtin_icon_for(const char *href) {
    if (!href) return NULL;
    if (strcmp(href, "shell:settings") == 0) return icon_builtin_settings;
    if (strcmp(href, "builtin:store")  == 0) return icon_builtin_store;
    if (strcmp(href, "builtin:notif")  == 0) return icon_builtin_notif;
    if (strcmp(href, "builtin:quick")  == 0) return icon_builtin_quick;
    if (strcmp(href, "builtin:notes")  == 0) return icon_builtin_notes;
    if (strcmp(href, "builtin:sysmon") == 0) return icon_builtin_sysmon;
    if (strcmp(href, "builtin:idqr")   == 0) return icon_builtin_identity;
    if (strcmp(href, "builtin:nfc")    == 0) return icon_builtin_nfc;
    return NULL;
}
EOF
} > "$OUT"

echo "wrote $OUT  ($(wc -l < "$OUT") lines, $(wc -c < "$OUT") bytes)"
