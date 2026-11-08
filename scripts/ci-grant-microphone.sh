#!/bin/bash
# CI only: best-effort microphone (input) permission for headless test runs.
# macOS returns silence, not an error, when a process without microphone
# permission reads from any input device, including virtual ones. This grants
# kTCCServiceMicrophone to the given binaries and to every ancestor process of
# this shell (the "responsible process" that TCC checks).
# Never run this on a personal Mac.
set -uo pipefail

clients=("$@")
pid=$$
while [ "$pid" -gt 1 ]; do
  path="$(ps -o comm= -p "$pid" 2>/dev/null | sed 's/^ *//')"
  [[ "$path" == /* ]] && clients+=("$path")
  pid="$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')"
  [ -z "$pid" ] && break
done

now="$(date +%s)"

grant() {
  local db="$1" client="$2" sudo_cmd="$3"
  [ -f "$db" ] || { echo "skip: $db not found"; return; }
  local cols values=""
  cols="$($sudo_cmd sqlite3 "$db" "SELECT name FROM pragma_table_info('access');" 2>/dev/null)" || { echo "skip: cannot read $db"; return; }
  while read -r col; do
    case "$col" in
      service) v="'kTCCServiceMicrophone'" ;;
      client) v="'$client'" ;;
      client_type) v="1" ;;
      auth_value) v="2" ;;
      auth_reason) v="4" ;;
      auth_version) v="1" ;;
      indirect_object_identifier_type) v="0" ;;
      indirect_object_identifier) v="'UNUSED'" ;;
      boot_uuid) v="'UNUSED'" ;;
      flags) v="0" ;;
      last_modified|last_reminded) v="$now" ;;
      *) v="NULL" ;;
    esac
    values+="${values:+,}$v"
  done <<< "$cols"
  if $sudo_cmd sqlite3 "$db" "INSERT OR REPLACE INTO access VALUES($values);" 2>/dev/null; then
    echo "granted: $client ($db)"
  else
    echo "failed:  $client ($db)"
  fi
}

for c in $(printf '%s\n' "${clients[@]}" | sort -u); do
  grant "$HOME/Library/Application Support/com.apple.TCC/TCC.db" "$c" ""
  grant "/Library/Application Support/com.apple.TCC/TCC.db" "$c" "sudo"
done
exit 0
