#!/bin/sh
# Installs a release package: the binary looks for its themes in /usr/local/share/isekur.
#   sudo ./install.sh
set -e
cd "$(dirname "$0")"
P=/usr/local
install -d "$P/bin" "$P/share/isekur/themes" "$P/share/man/man1"
install -m 755 isekur "$P/bin/isekur"
install -m 644 isekur.1 "$P/share/man/man1/isekur.1"
install -m 644 themes/*.theme "$P/share/isekur/themes/"
install -m 644 iSekur.conf.example README.md LICENSE "$P/share/isekur/"
echo "installed $P/bin/isekur"
