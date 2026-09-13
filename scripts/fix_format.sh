#!/usr/bin/env bash
set -eux
find ./src/ '(' -iname '*.cc' -o -iname '*.h' ')' -print | xargs clang-format -i --verbose
