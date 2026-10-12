#!/bin/sh
# fail2ban-contract v1: this role is VENDORED, byte-identical, in
#   abulxt      deploy/ansible/roles/fail2ban
#   KrunkNative deploy/ansible/roles/fail2ban
# Change both copies together, re-stamp VERSION, then check:
#
#   check-vendored.sh                  VERSION's sha matches this copy's files
#   check-vendored.sh OTHER_ROLE_DIR   ... and the other repo's copy is identical
#   check-vendored.sh --stamp          rewrite VERSION's sha after an edit
#
# The sha covers every file in the role except VERSION (sorted relative paths +
# contents, sha256). Exit 0 = OK, 1 = mismatch.
set -eu
here=$(cd "$(dirname "$0")" && pwd)

role_sha() {
	(cd "$1" && find . -type f ! -name VERSION | LC_ALL=C sort | while read -r f; do
		printf '%s\n' "$f"
		cat "$f"
	done) | sha256sum | cut -d' ' -f1
}

if [ "${1:-}" = "--stamp" ]; then
	printf 'fail2ban-contract v1\nsha256 %s\n' "$(role_sha "$here")" > "$here/VERSION"
	cat "$here/VERSION"
	exit 0
fi

rc=0
want=$(sed -n 's/^sha256 //p' "$here/VERSION")
have=$(role_sha "$here")
if [ "$want" = "$have" ]; then
	echo "OK   $here matches VERSION ($have)"
else
	echo "FAIL $here does not match VERSION (VERSION $want, files $have): run --stamp" >&2
	rc=1
fi

if [ $# -ge 1 ]; then
	other=$(cd "$1" && pwd)
	if diff -r "$here" "$other"; then
		echo "OK   $other is identical"
	else
		echo "FAIL $other differs from $here (update both repos together)" >&2
		rc=1
	fi
fi
exit $rc
