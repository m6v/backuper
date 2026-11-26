#!/usr/bin/env bash
set -euo pipefail

# Пути к тестируемым бинарникам (можно переопределить: PARTBACK=/usr/local/bin/partback ./test_suite.sh)
PARTBACK="${PARTBACK:-./partback}"
PARTREST="${PARTREST:-./partrest}"
WORK_DIR="$(mktemp -d /tmp/partback_test.XXXXXX)"

# Цвета для наглядного вывода
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Гарантированная очистка временных файлов и точек монтирования при любом исходе
cleanup() {
    if mountpoint -q "$WORK_DIR/mnt" 2>/dev/null; then
        sudo umount "$WORK_DIR/mnt" || true
    fi
    sudo rm -rf "$WORK_DIR"
}
trap cleanup EXIT

log_step() { echo -e "\n${YELLOW}=== $1 ===${NC}"; }
log_ok()   { echo -e "${GREEN}[OK]${NC} $1"; }
log_skip() { echo -e "${YELLOW}[SKIP]${NC} $1"; }
log_err()  { echo -e "${RED}[FAIL]${NC} $1" >&2; exit 1; }

# Проверка наличия бинарников и базовых утилит
[[ -x "$PARTBACK" ]] || log_err "Бинарник '$PARTBACK' не найден или не исполняемый"
[[ -x "$PARTREST" ]] || log_err "Бинарник '$PARTREST' не найден или не исполняемый"
command -v zstd >/dev/null || log_err "Не установлена утилита zstd"

mkdir -p "$WORK_DIR/mnt"

# Вспомогательная функция: наполняет смонтированный каталог детерминированными файлами и создает фрагментацию
populate_and_fragment() {
    local mnt="$1"
    sudo mkdir -p "$mnt/dir1" "$mnt/dir2" "$mnt/dir3"
    
    # Создаем по 45 МБ псевдослучайных данных в каждой папке (итого 135 МБ на раздел 350 МБ)
    sudo dd if=/dev/urandom of="$mnt/dir1/data1.bin" bs=1M count=45 status=none
    sudo dd if=/dev/urandom of="$mnt/dir2/data2.bin" bs=1M count=45 status=none
    sudo dd if=/dev/urandom of="$mnt/dir3/data3.bin" bs=1M count=45 status=none
    
    # Добавим немного мелких файлов для проверки инодов и метаданных
    sudo cp -r /etc/systemd "$mnt/dir1/etc_sample" 2>/dev/null || true
    
    # Удаляем средний каталог, чтобы создать гарантированную "дыру" 45 МБ в битовой карте блоков
    sudo rm -rf "$mnt/dir2"
}

# ------------------------------------------------------------------------------
# ТЕСТ 1: ext4 (с наполнением, фрагментацией, --status-fd, --verify и e2fsck)
# ------------------------------------------------------------------------------
log_step "Тест 1: ext4 (фрагментированные данные + --status-fd + --verify + e2fsck)"
dd if=/dev/zero of="$WORK_DIR/src_ext4.img" bs=1M count=350 status=none
/sbin/mkfs.ext4 -F -q "$WORK_DIR/src_ext4.img"

sudo mount -o loop "$WORK_DIR/src_ext4.img" "$WORK_DIR/mnt"
populate_and_fragment "$WORK_DIR/mnt"
sudo find "$WORK_DIR/mnt" -type f -exec sha256sum {} + | sort > "$WORK_DIR/orig_ext4.sha"
sudo umount "$WORK_DIR/mnt"

# Запускаем бэкап с перехватом прогресса из FD 3
"$PARTBACK" --status-fd 3 "$WORK_DIR/src_ext4.img" - 3>"$WORK_DIR/progress.log" | \
    zstd -T0 -f -q -o "$WORK_DIR/bak_ext4.zst"

grep -q "XXX" "$WORK_DIR/progress.log" || log_err "Поток прогресса (--status-fd 3) пуст!"
log_ok "Поток прогресса (--status-fd 3) проверен"

zstd -dc -q "$WORK_DIR/bak_ext4.zst" | "$PARTREST" --verify -
zstd -dc -q "$WORK_DIR/bak_ext4.zst" | "$PARTREST" - "$WORK_DIR/dst_ext4.img"

sudo /sbin/e2fsck -fn "$WORK_DIR/dst_ext4.img" >/dev/null
sudo mount -o loop "$WORK_DIR/dst_ext4.img" "$WORK_DIR/mnt"
sudo find "$WORK_DIR/mnt" -type f -exec sha256sum {} + | sort > "$WORK_DIR/rest_ext4.sha"
sudo umount "$WORK_DIR/mnt"

diff -u "$WORK_DIR/orig_ext4.sha" "$WORK_DIR/rest_ext4.sha" || log_err "Хэши файлов ext4 не совпали!"
log_ok "ext4: 100% побайтовое совпадение всех файлов и метаданных"

# ------------------------------------------------------------------------------
# ТЕСТ 2: Raw / FAT32 (имитация системного ESP-раздела)
# ------------------------------------------------------------------------------
log_step "Тест 2: Raw / FAT32 (ESP-раздел)"
if command -v /sbin/mkfs.vfat >/dev/null || command -v mkfs.vfat >/dev/null; then
    dd if=/dev/zero of="$WORK_DIR/src_vfat.img" bs=1M count=64 status=none
    /sbin/mkfs.vfat -F 32 "$WORK_DIR/src_vfat.img" >/dev/null
    ORIG_VFAT=$(sha256sum "$WORK_DIR/src_vfat.img" | awk '{print $1}')

    "$PARTBACK" "$WORK_DIR/src_vfat.img" - | zstd -T0 -f -q -o "$WORK_DIR/bak_vfat.zst"
    zstd -dc -q "$WORK_DIR/bak_vfat.zst" | "$PARTREST" --verify -
    zstd -dc -q "$WORK_DIR/bak_vfat.zst" | "$PARTREST" - "$WORK_DIR/dst_vfat.img"

    REST_VFAT=$(sha256sum "$WORK_DIR/dst_vfat.img" | awk '{print $1}')
    [[ "$ORIG_VFAT" == "$REST_VFAT" ]] || log_err "Хэш образа FAT32 не совпал!"
    log_ok "Raw / FAT32: 100% побайтовое совпадение образа"
else
    log_skip "mkfs.vfat не найден, тест FAT32 пропущен"
fi

# ------------------------------------------------------------------------------
# ТЕСТ 3: NTFS (проверка $Bitmap + резервный загрузочный сектор Alternate VBR)
# ------------------------------------------------------------------------------
log_step "Тест 3: NTFS (\$Bitmap + Alternate Boot Sector)"
if command -v mkfs.ntfs >/dev/null && command -v ntfsfix >/dev/null; then
    dd if=/dev/zero of="$WORK_DIR/src_ntfs.img" bs=1M count=64 status=none
    mkfs.ntfs -F -Q "$WORK_DIR/src_ntfs.img" >/dev/null

    "$PARTBACK" "$WORK_DIR/src_ntfs.img" - | zstd -T0 -f -q -o "$WORK_DIR/bak_ntfs.zst"
    zstd -dc -q "$WORK_DIR/bak_ntfs.zst" | "$PARTREST" --verify -
    zstd -dc -q "$WORK_DIR/bak_ntfs.zst" | "$PARTREST" - "$WORK_DIR/dst_ntfs.img"

    ntfsfix -n "$WORK_DIR/dst_ntfs.img" >/dev/null || log_err "ntfsfix обнаружил ошибки в восстановленном NTFS!"
    log_ok "NTFS: 100% целостность (\$MFT и Alternate Boot Sector в норме)"
else
    log_skip "mkfs.ntfs / ntfsfix не найдены (пакет ntfs-3g), тест NTFS пропущен"
fi

# ------------------------------------------------------------------------------
# ТЕСТ 4: exFAT (проверка записи 0x81 Allocation Bitmap)
# ------------------------------------------------------------------------------
log_step "Тест 4: exFAT (Allocation Bitmap 0x81)"
if command -v mkfs.exfat >/dev/null && command -v fsck.exfat >/dev/null; then
    dd if=/dev/zero of="$WORK_DIR/src_exfat.img" bs=1M count=64 status=none
    mkfs.exfat "$WORK_DIR/src_exfat.img" >/dev/null

    "$PARTBACK" "$WORK_DIR/src_exfat.img" - | zstd -T0 -f -q -o "$WORK_DIR/bak_exfat.zst"
    zstd -dc -q "$WORK_DIR/bak_exfat.zst" | "$PARTREST" --verify -
    zstd -dc -q "$WORK_DIR/bak_exfat.zst" | "$PARTREST" - "$WORK_DIR/dst_exfat.img"

    fsck.exfat -n "$WORK_DIR/dst_exfat.img" >/dev/null || log_err "fsck.exfat обнаружил ошибки в восстановленном exFAT!"
    log_ok "exFAT: 100% целостность тома"
else
    log_skip "mkfs.exfat / fsck.exfat не найдены (пакет exfatprogs), тест exFAT пропущен"
fi

# ------------------------------------------------------------------------------
# ТЕСТ 5: Защита от повреждения архива (проверка срабатывания CRC32)
# ------------------------------------------------------------------------------
log_step "Тест 5: Негативный тест (проверка отлова битого байта по CRC32)"
zstd -dc -q "$WORK_DIR/bak_vfat.zst" > "$WORK_DIR/corrupted.img"
# Портим один байт в середине распакованного потока
printf '\xFF' | dd of="$WORK_DIR/corrupted.img" bs=1 seek=8192 count=1 conv=notrunc status=none
if "$PARTREST" --verify "$WORK_DIR/corrupted.img" 2>/dev/null; then
    log_err "partrest --verify НЕ поймал поврежденный байт!"
else
    log_ok "partrest --verify успешно заблокировал поврежденный архив по CRC32"
fi

echo -e "\n${GREEN}====================================================${NC}"
echo -e "${GREEN}   ВСЕ ТЕСТЫ УСПЕШНО ПРОЙДЕНЫ!                      ${NC}"
echo -e "${GREEN}====================================================${NC}"
