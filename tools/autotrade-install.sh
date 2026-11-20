#!/usr/bin/env bash
# Установка реальной торговли автокопирования (один раз, владелец, с sudo):
#
#   sudo bash ~/WhaleScanner/tools/autotrade-install.sh
#
# Что делает: кладёт службу whale-autotrade, создаёт /etc/whale-autotrade.env
# (root, 600) с пустыми полями, если его нет, и каталог модуля. Ключи
# вписывает владелец сам: sudoedit /etc/whale-autotrade.env — в чат и в код
# их не давать. Служба запускается, но без нажатия «Старт» во вкладке не
# торгует; без ключей — пишет во вкладку «нет ключа OKX».
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "нужно через sudo"; exit 1; }
USER_NAME=${WT_USER:-vadymdota2}
HOME_DIR=/home/$USER_NAME
UNIT_SRC=$HOME_DIR/WhaleScanner/deploy/whale-autotrade.service
ENV=/etc/whale-autotrade.env
DATA=$HOME_DIR/WalletTracker/autotrade_data

install -m 644 "$UNIT_SRC" /etc/systemd/system/whale-autotrade.service
if [ ! -f "$ENV" ]; then
    umask 077
    cat >"$ENV" <<'TPL'
# Ключ OKX для автокопирования. Права ключа: ТОЛЬКО «Торговля» (без вывода),
# привязка к IP сервера. Лучше — на отдельном субаккаунте.
OKX_API_KEY=
OKX_API_SECRET=
OKX_API_PASSPHRASE=
# 1 — демо-счёт OKX (ключ тоже должен быть демо), 0 — реальный счёт.
OKX_SIMULATED=0
TPL
    echo "создан $ENV — впишите ключ: sudoedit $ENV"
fi
chown root:root "$ENV"
chmod 600 "$ENV"
install -d -o "$USER_NAME" -g "$USER_NAME" -m 755 "$DATA" "$DATA/live"
systemctl daemon-reload
systemctl enable whale-autotrade >/dev/null
systemctl restart whale-autotrade
sleep 3
systemctl --no-pager --lines=5 status whale-autotrade || true
echo "готово. Ключ: sudoedit $ENV, затем sudo systemctl restart whale-autotrade. Торговля — после «Старт» во вкладке."
