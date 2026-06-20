#!/bin/sh
# Attach compiled resources to the installed NvidiaGPU binary and register its
# MIME type. Invoked by meson at install time; degrades gracefully when the
# Haiku resource tools are unavailable.
set -e

rsrc="$1"
exe="$MESON_INSTALL_DESTDIR_PREFIX/apps/Nebula/NvidiaGPU"

if [ ! -f "$exe" ]; then
	exit 0
fi

if command -v xres >/dev/null 2>&1; then
	xres -o "$exe" "$rsrc"
fi

if command -v settype >/dev/null 2>&1; then
	settype -t application/x-vnd.Be-elfexecutable "$exe"
fi

if command -v mimeset >/dev/null 2>&1; then
	mimeset -f "$exe"
fi
