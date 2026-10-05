#!/usr/bin/env bash
# Checks that tools/release.sh refuses bad input. Never creates or pushes a tag.
cd "$(dirname "$0")/.."
fail=0
expect_refusal() {   # description, expected stderr fragment, args...
  local what=$1 frag=$2; shift 2
  out=$(tools/release.sh "$@" 2>&1); rc=$?
  if [ $rc -eq 0 ] || ! grep -q -- "$frag" <<<"$out"; then echo "FAIL: $what (rc=$rc): $out"; fail=1; else echo "ok: $what"; fi
}
expect_refusal "no tag"                "usage"            --yes
expect_refusal "tag without v"         "is not vMAJOR"    1.2.3 --yes
expect_refusal "tag missing patch"     "is not vMAJOR"    v1.2 --yes
expect_refusal "tag with junk suffix"  "is not vMAJOR"    'v1.2.3-bad!' --yes
expect_refusal "unknown option"        "unknown option"   v1.2.3 --bogus
if [ "$(git branch --show-current)" != main ]; then
  expect_refusal "not on main"         "not main"         v9.9.9 --yes
fi
exit $fail
