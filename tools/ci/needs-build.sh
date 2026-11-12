#!/usr/bin/env bash
# Says whether a CI run has to build and test, in GITHUB_OUTPUT as build=true|false.
# A pull request (or a push to main) that only changes documentation -- the
# CHANGELOG, the READMEs, docs/ -- skips the build, but its jobs still run and pass,
# so the branch protection's required checks are reported and nothing is left
# waiting. Tags, manual runs and anything it cannot tell always build.
set -u

case "${GITHUB_EVENT_NAME:-}" in
  pull_request) range="${BASE_SHA:-}...${HEAD_SHA:-}" ;;
  push)
    if [[ "${GITHUB_REF:-}" == refs/heads/* && -n "${BEFORE_SHA:-}" && ! "${BEFORE_SHA}" =~ ^0+$ ]]; then
      range="${BEFORE_SHA}..${GITHUB_SHA}"
    else
      range=""
    fi ;;
  *) range="" ;;
esac

build=true
if [[ -n "$range" ]] && files=$(git diff --name-only "$range" 2>/dev/null) && [[ -n "$files" ]]; then
  # Documentation: anything in docs/, and .md files (in tests/, only the READMEs).
  code=$(printf '%s\n' "$files" | awk '
    /^tests\// { if ($0 !~ /README\.md$/) print; next }
    /^docs\// || /\.md$/ { next }
    { print }')
  if [[ -z "$code" ]]; then
    build=false
    echo "::notice::Only documentation changed; skipping the build and the tests:"
    printf '  %s\n' $files
  fi
fi
echo "build=$build" >> "${GITHUB_OUTPUT:-/dev/stdout}"
