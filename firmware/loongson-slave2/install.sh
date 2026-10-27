#!/bin/sh
set -eu
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo 'Run as root' >&2; exit 1; }
[ "$(uname -m)" = loongarch64 ] || { echo 'This package targets loongarch64' >&2; exit 1; }
sha256sum -c SHA256SUMS
mkdir -p /var/lib/loongson-slave2 /usr/local/sbin /etc/systemd/system
backup=$(mktemp -d /var/lib/loongson-slave2/backup-XXXXXX)
for file in loongson-slave2 loongson-slave2-reboot; do
    if [ -f "/usr/local/sbin/$file" ]; then cp -p "/usr/local/sbin/$file" "$backup/$file"; fi
done
if [ -f /etc/systemd/system/loongson-slave2.service ]; then
    cp -p /etc/systemd/system/loongson-slave2.service "$backup/loongson-slave2.service"
fi
rollback() {
    trap - EXIT HUP INT TERM
    systemctl stop loongson-slave2 || true
    for file in loongson-slave2 loongson-slave2-reboot; do
        if [ -f "$backup/$file" ]; then cp -p "$backup/$file" "/usr/local/sbin/$file.rollback"; mv "/usr/local/sbin/$file.rollback" "/usr/local/sbin/$file"; fi
    done
    if [ -f "$backup/loongson-slave2.service" ]; then
        cp -p "$backup/loongson-slave2.service" /etc/systemd/system/loongson-slave2.service
        systemctl daemon-reload; systemctl start loongson-slave2 || true
    fi
    echo "Install failed; previous files restored when available: $backup" >&2
}
trap rollback EXIT HUP INT TERM
if systemctl cat loongson-slave2 >/dev/null 2>&1; then systemctl stop loongson-slave2; fi
for file in loongson-slave2 loongson-slave2-reboot; do
    cp "$file" "/usr/local/sbin/$file.new"; chmod 755 "/usr/local/sbin/$file.new"; mv "/usr/local/sbin/$file.new" "/usr/local/sbin/$file"
done
cp loongson-slave2.service /etc/systemd/system/loongson-slave2.service
chmod 644 /etc/systemd/system/loongson-slave2.service
systemctl daemon-reload
systemctl start loongson-slave2
systemctl is-active --quiet loongson-slave2
systemctl enable loongson-slave2
trap - EXIT HUP INT TERM
echo "Installed; backup: $backup"
