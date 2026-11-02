#!/usr/bin/env bash
set -euo pipefail

[ $# -eq 2 ] || { echo "usage: native-deps.sh <release-prefix> <destination>" >&2; exit 2; }
prefix=$1
destination=$2
repository=chiaki-ng/native-deps

auth=()
if [ -n "${GITHUB_TOKEN:-}" ]; then
  auth=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
fi

tag=$(curl -fsSL "${auth[@]}" "https://api.github.com/repos/${repository}/releases?per_page=100" \
  | jq -r --arg prefix "$prefix" \
      '[.[] | select(.tag_name | startswith($prefix))] | max_by(.published_at) | .tag_name // empty')
[ -n "$tag" ] || { echo "no ${prefix}* release in ${repository}" >&2; exit 1; }

mkdir -p "$destination"
cd "$destination"
for asset in build.tar.gz build.tar.gz.sha256; do
  curl -fsSL -o "$asset" "https://github.com/${repository}/releases/download/${tag}/${asset}"
done
expected=$(cut -d' ' -f1 build.tar.gz.sha256)
actual=$( (sha256sum build.tar.gz 2>/dev/null || shasum -a 256 build.tar.gz) | cut -d' ' -f1)
[ "$expected" = "$actual" ] || { echo "${tag}: build.tar.gz does not match its published SHA-256" >&2; exit 1; }
tar -xzf build.tar.gz
rm build.tar.gz build.tar.gz.sha256
echo "$tag" >TAG

pool=$( (sed -n 's#^prefix=\(/.*\)/[^/]*$#\1#p' ./*/lib/pkgconfig/*.pc 2>/dev/null || true) | head -n 1)
[ -n "$pool" ] || { echo "${tag}: no pkg-config file says where the archive was built" >&2; exit 1; }
if [ "$pool" != "$PWD" ]; then
  { grep -rlIF "$pool" . || true; } | while IFS= read -r text; do
    perl -pi -e "s#\\Q${pool}\\E#${PWD}#g" "$text"
  done
  if command -v install_name_tool >/dev/null; then
    find . -type f -name '*.dylib' | while IFS= read -r library; do
      changes=()
      identity=$(otool -D "$library" | sed -n 2p)
      if [[ "$identity" == "$pool"/* ]]; then
        changes+=(-id "${PWD}${identity#"$pool"}")
      fi
      while IFS= read -r linked; do
        if [[ "$linked" == "$pool"/* ]]; then
          changes+=(-change "$linked" "${PWD}${linked#"$pool"}")
        fi
      done < <(otool -L "$library" | tail -n +2 | awk '{print $1}')
      if [ ${#changes[@]} -gt 0 ]; then
        install_name_tool "${changes[@]}" "$library"
        codesign --force --sign - "$library" 2>/dev/null
      fi
    done
  fi
fi

find "$PWD" -mindepth 2 -maxdepth 2 -name BUILD-INFO -exec dirname {} \; | sort | paste -sd';' -
