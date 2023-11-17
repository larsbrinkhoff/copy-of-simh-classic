#!/bin/sh

SITE="http://simh.trailing-edge.com"
SOURCES="$SITE/sources"
ARCHIVE="$SOURCES/archive"

download() {
    while read i; do
        set $i
        wget "$ARCHIVE/$1.zip" || wget "$SOURCES/$1.zip"
    done
}

rm -f *.zip*
download < versions
