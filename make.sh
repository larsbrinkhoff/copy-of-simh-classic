#!/bin/sh

set -x

AUTHOR="Bob Supnik <bob@supnik.org>"
HERE="$PWD"

cleanup() {
    cd "$HERE"
    rm -rf tmp
}

commit() {
    version="$1"
    echo "$version"
    rm -rf * .gitignore
    unzip -bo "../$version.zip"
    test -d sim && mv sim/* .
    git add -A .
    git commit --author "$AUTHOR" -m "$version"
}

trap cleanup EXIT INT QUIT TERM
git branch -D master
rm -rf tmp
git clone . tmp
cd tmp
git checkout --orphan master
cat ../versions | while read i; do set $i; commit "$1"; done
git push origin master
