#!/usr/bin/env bash
# Signs a file for the release workflow's tests with the key made for them:
# minisign's, when the run made one with minisign (on one runner); else
# tests/upgrade/minisign.py's. `--public` prints that key's public half,
# for CLUAU_RELEASE_KEYS.
#
#   bash .github/sign-test.sh <file> <trusted comment>
#   bash .github/sign-test.sh --public
set -euo pipefail
seed=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
signer="$(dirname "$0")/../tests/upgrade/minisign.py"
if [ "${RUNNER_OS:-}" = Windows ]; then
	python=python
else
	python=python3
fi
if [ "$1" = --public ]; then
	if [ -f "${RUNNER_TEMP:-/nonexistent}/test.pub" ]; then
		tail -n 1 "$RUNNER_TEMP/test.pub"
	else
		"$python" "$signer" public "$seed"
	fi
elif [ -f "${RUNNER_TEMP:-/nonexistent}/test.key" ]; then
	minisign -S -s "$RUNNER_TEMP/test.key" -m "$1" -t "$2"
else
	"$python" "$signer" sign "$seed" "$1" "$2"
fi
