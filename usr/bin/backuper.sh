#!/bin/sh

BASE_DIR="$(cd $(dirname $0) && pwd)"
export PATH="$BASE_DIR:$PATH"

# Определение текущего каталога по реальному пути, игнорируя симлинки (флаг -P)
BASE_DIR="$(cd -P -- "$(dirname -- "$0")" && pwd -P)"
export PATH="$BASE_DIR:$PATH"

# Вычисление абсолютных путей к библиотекам относительно каталога скрипта
LIB_DIR="$BASE_DIR/../lib"
GLIBC_DIR="$BASE_DIR/../lib/x86_64-linux-gnu"

if [ -z "$LD_LIBRARY_PATH" ]; then
    export LD_LIBRARY_PATH="$LIB_DIR:$GLIBC_DIR"
else
    export LD_LIBRARY_PATH="$LIB_DIR:$GLIBC_DIR:$LD_LIBRARY_PATH"
fi

DEBUG=""
# Обертка для пропуска команд в режиме отладки
run() {
    if [ -n "$DEBUG" ]; then
        # Вывод команды вместо выполнения
        echo -e "\e[1;33m$*\e[0m"
    else
        # Выполнение команды
        eval "$@"
    fi
}

# Перевод размера диска (раздела) в секторах в ГБ
sectors2gb() {
    local sectors=$1
    [ -z "$sectors" ] && echo ""
    local gb_int=$(( sectors / 2097152 ))
    local gb_rem=$(( (sectors % 2097152) * 10 / 2097152 ))
    echo "${gb_int}.${gb_rem} ГБ"
}

# Диалог выбора дисков
select_disk_dialog() {
    set --

    for disk_path in /sys/block/*; do
        # Проверка, что это ссылка/директория, а не случайный файл
        [ -e "$disk_path" ] || continue

        disk_name=$(basename $disk_path)
        case $disk_name in
            sd*|nvme*|mmcblk*|hd*|vd*) ;;
            *) continue ;;
        esac

        # Чтение модели диска из device/model или device/name (для mmcblk)
        disk_model=""
        if [ -f "$disk_path/device/model" ]; then
            disk_model=$(cat "$disk_path/device/model" 2>/dev/null)
        elif [ -f "$disk_path/device/name" ]; then
            disk_model=$(cat "$disk_path/device/name" 2>/dev/null)
        fi
        # Удаление лишних пробелов в начале/конце
        disk_model=$(echo $disk_model) 
        [ -z "$disk_model" ] && model="Unknown"

        disk_size=$(sectors2gb $(cat /sys/block/$disk_name/size))

        # Добавление дуплета в аргументы для whiptail
        set -- "$@" "/dev/$disk_name" "$disk_model ($disk_size)"
    done

    [ $# -eq 0 ] && return 1

    selection=$(whiptail --title "Выбор диска" --menu "Выберите диск (Enter - OK):" 0 60 0 "$@" 3>&1 1>&2 2>&3)
    # Возврат выбранного диска, если нажата <OK>
    if [ $? -eq 0 ] && [ -n "$selection" ]; then
        echo "$selection"
        return 0
    else
        return 1
    fi
}

# Диалог выбора разделов
select_partitions_dialog() {
    local disk_name=$(basename $1)

    # Проверка наличия диска в системе
    if [ ! -d "/sys/block/$disk_name" ]; then
        return 1
    fi

    # Очистка позиционных параметров
    set -- 

    # Счетчик разделов
    local part_count=0

    # Перебираем папки разделов внутри /sys/block/имя_диска/
    for part_dir in /sys/block/"$disk_name"/"$disk_name"* /sys/block/"$disk_name"/p[0-9]*; do
        [ ! -d "$part_dir" ] && continue

        local part_name=$(basename $part_dir)
        local partition="/dev/$part_name"

        # Определение размера раздела
        size_str=$(sectors2gb $(cat "$part_dir/size" 2>/dev/null))

        # Определение типа раздела
        local part_type=""
        if [ -f "$part_dir/uevent" ]; then
            # Основной способ
            part_type=$(grep -E "PARTNAME|PARTTYPE_NAME" "$part_dir/uevent" | head -n 1 | cut -d'=' -f2)
            # Запасной способ
            if [ -z "$part_type" ]; then
                part_type=$(blkid "$partition" 2>/dev/null | grep -o 'TYPE="[^"]*"' | cut -d'"' -f2)
            fi
        fi
        [ -z "$part_type" ] && part_type="Unknown"

        # Добавление триплета в аргументы для whiptail
        set -- "$@" "$partition" "$part_type ($size_str)" "ON"
        part_count=$(( part_count + 1 ))
    done

    if [ "$part_count" -eq 0 ]; then
        return 0
    fi

    # Установка высоты внутренней рамки и окна
    local list_height=$part_count
    local window_height=$(( part_count + 7 ))

    if [ "$list_height" -gt 10 ]; then
        list_height=10
        window_height=17
    fi

    export NEWT_COLORS='
        listbox=black,lightgray
        sellistbox=black,lightgray
        actsellistbox=white,black
        checkbox=black,lightgray
        actcheckbox=lightgray,black
    '

    local selections
    selections=$(whiptail --title "Выбор разделов" \
        --checklist "Выберите разделы (Пробел - выбор, Enter - ОК):" \
        $window_height 60 $list_height \
        --separate-output \
        "$@" 3>&1 1>&2 2>&3)

    if [ $? -eq 0 ] && [ -n "$selections" ]; then
        echo "$selections"
        return 0
    else
        return 1
    fi
}

# Тест диалогов выбора дисков и разделов
if false; then
    DISK=$(select_disk_dialog)
    echo $DISK
    if [ -n "$DISK" ]; then
        select_partitions_dialog $DISK
    fi
exit 0
fi

backup_gpt() {
    DISK=$1
    echo "Backing up GPT of $DISK..."
    run 'mkdir -p $STORAGE'
    IMAGE=$STORAGE/$(basename $DISK).sf

    if ! run 'sfdisk -d $DISK > $IMAGE'; then
        echo "Failed to dump GPT"
        return 1
    fi
    echo "GPT dump saved to $IMAGE"

    IMAGE=$STORAGE/$(basename $DISK).fd
    run 'fdisk -l "$DISK" > $IMAGE'
    echo "Partitions info saved to $IMAGE"
}

restore_gpt() {
    DISK=$(select_disk_dialog)
    if [ $? -ne 0 ]; then
        echo "Restoring GUID partition table canceled"
        return 1;
    fi

    IMAGE=$STORAGE/$(basename $DISK).sf
    echo "Restoring GPT of $DISK..."

    if mount | grep -q ^$DISK; then
        echo "Device $DISK is busy"
        return 1
    fi
    if [ ! -f $IMAGE ]; then
        echo "GPT dump $IMAGE not found"
        return 2
    fi
    
    whiptail --title "Внимание" --yesno "Все данные диске будут уничтожены! Продолжить?" 8 60
    if [ $? -ne 0 ]; then
        echo "Operation canceled by user"
        return 1
    fi
    
    # Восстановление GPT
    if run 'sfdisk $DISK < $IMAGE'; then
        # Синхронизация геометрии диска с ядром Linux 
        run 'blockdev --rereadpt "$DISK"'
        # Принудительное пересоздание файлов разделов в /dev
        run 'udevadm trigger --settle'
        sleep 2
        echo "GPT restored successfully"
        return 0
    fi
    echo "Failed to restore GPT"
}

backup_part() {
    DISK=$1
    PARTITION=$2
    if mount | grep -q $PARTITION; then
        echo "Partition $PARTITION is mounted. Skipping backup"
        return 1
    fi

    run 'mkdir -p $STORAGE'
    IMAGE=$STORAGE/$(basename $PARTITION).qcow2

    part_info=$(blkid | grep $PARTITION)
    part_type=$(echo "$part_info" | sed 's/.*TYPE="\([^"]*\).*/\1/')

    case "$part_type" in
        swap)
            echo "Skipped swap $PARTITION"
            ;;
        *)
            run 'mkdir -p /tmp/trim'
            run 'mount $PARTITION /tmp/trim'

            # Проверка поддержки диском триммирования
            if [ "$(cat /sys/block/$(basename $DISK)/queue/discard_max_bytes 2>/dev/null)" -gt 0 ]; then
                echo "Trimming free space on $PARTITION..."
                run 'fstrim -v /tmp/trim 2>/dev/null || true'
            else
                # NB! Для vfat при превышении размера файла 4ГБ завершится с ошибкой "File too large"
                echo "Writing zeroes to clear free space on $PARTITION..."
                run 'dd if=/dev/zero of=/tmp/trim/zero bs=4M status=progress 2>/dev/null || true'
                run 'rm /tmp/trim/zero'
            fi
            run 'umount /tmp/trim'
            run 'sync'
            run 'udevadm settle'

            echo "Backuping $PARTITION..."
            run 'qemu-img convert -p -W -c -o compression_type=zstd -O qcow2 $PARTITION $IMAGE'
            ;;
    esac
    if [ -f $IMAGE ]; then
        run 'qemu-img check $IMAGE'
    fi
}

restore_part() {
    DISK=$1
    PARTITION=$2
    if mount | grep -q $PARTITION; then
        echo "Partition $PARTITION is mounted. Skipped"
        return 1
    fi

    # Для свопа выполнение форматирования вместо копировния
    part_info=$(blkid | grep $PARTITION)
    part_type=$(echo "$part_info" | sed 's/.*TYPE="\([^"]*\).*/\1/')
    if [ part_type = "swap" ]; then
        echo "Formatting swap partition $PARTITION..."
        run 'mkswap $PARTITION'
        return 0
    fi

    IMAGE=$STORAGE/$(basename $PARTITION).qcow2
    if [ ! -f $IMAGE ]; then
        echo "Image $IMAGE is not found"
        return 2
    fi

    # Определение типа диска и установка доп. параметра
    if [ $(cat /sys/block/$(basename $DISK)/queue/rotational) -eq 0 ]; then
        # Это SSD
        echo "Discard blocks on $PARTITION"
        run 'blkdiscard $PARTITION'
        parm=""
    else
        # Это HDD
        echo "Wipe signatures on $PARTITION"
        dd if=/dev/zero of="$PARTITION" bs=1M count=16 status=none
        sync
        # Disable sparse detection to enforce fast sequential write on HDD
        parm="-S 0"
    fi
    echo "Restoring partition $PARTITION..."
    run 'qemu-img convert -p -O raw $IMAGE $PARTITION'

    # Добавление записи в NVRAM, если восстанавливаемый раздел это ESP
    PARTITION_INFO=$(sfdisk -d $DISK | grep $PARTITION.*C12A7328)
    if [ -z "$PARTITION_INFO" ]; then
        return 0
    fi

    PARTITION_NUMBER="${PARTITION##*[a-zA-Z]}"
    PARTITION_UUID="${PARTITION_INFO#*uuid=}"
    if ! efibootmgr -v | grep -iq $PARTITION_UUID; then
        run 'efibootmgr -c -d $DISK -p $PARTITION_NUMBER -L "astralinux" -l "\EFI\astralinux\grubx64.efi"'
        echo "Boot entry added to NVRAM succefully"
    else
        echo "Boot entry already exists in NVRAM. Skipped"
    fi
}

backup() {
    if ! DISK=$(select_disk_dialog) || ! PARTITIONS=$(select_partitions_dialog "$DISK"); then
        echo "Operation cancelled by user"
        return 1
    fi

    if [ -z "$PARTITIONS" ]; then
        echo "No partitions selected. Operation cancelled"
        return 1
    fi

    backup_gpt  "$DISK"
    whiptail --title "Создание образов разделов" --infobox "Ждите завершения операции. Прогресс в строке состояния" 7 60
    # Передача списка строк в цикл построчно
    echo "$PARTITIONS" | while IFS= read -r PART; do
        backup_part $DISK $PART
    done
}

restore() {
    if ! DISK=$(select_disk_dialog) || ! PARTITIONS=$(select_partitions_dialog "$DISK"); then
        echo "Operation canceled by user"
        return 1
    fi

    if [ -z $PARTITIONS ]; then
        echo "No partitions found on $DISK. Please restore GPT from dump"
        whiptail --title "Внимание" --infobox "На диске $DISK нет разделов. Восстановите таблицу разделов (GPT)" 7 60
        return 1
    fi

    if [ -z "$PARTITIONS" ]; then
        echo "No partitions selected. Operation cancelled"
        return 1
    fi

    whiptail --title "Внимание" --yesno "Все данные на восстанавливаемых разделах будут уничтожены! Продолжить?" 8 60
    if [ $? -ne 0 ]; then
        echo "Operation canceled by user"
        return 1
    fi

    whiptail --title "Восстановление разделов из образов" --infobox "Ждите завершения операции. Прогресс в строке состояния" 7 60
    # Передача списка строк в цикл построчно
    echo "$PARTITIONS" | while IFS= read -r PART; do
        restore_part $DISK $PART
    done

    echo "Forcing kernel to rescan partition table and refresh cache..."
    echo "change" > /sys/class/block/$(basename $DISK)/uevent
}

while true; do
    HOSTNAME=$(whiptail --title "Ввод имени хоста" --inputbox "Введите имя хоста:" 10 60 "$(hostname)" 3>&1 1>&2 2>&3)
    if [ $? -ne 0 ]; then exit 1; fi

    if echo $HOSTNAME | grep -qxE '^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$'; then
        break
    fi
   whiptail --title "Error" --msgbox "Недопустимое имя хоста '$HOSTNAME'. Повторите ввод" 0 60
done

# Каталог для хранения образов хоста
STORAGE="../../$BASE_DIR/$HOSTNAME"

while true; do
    menu=$(whiptail --title "Главное меню" --menu "Выбор действия для $HOSTNAME:" 0 60 0 \
        "1" "Создать образы разделов" \
        "2" "Восстановить разделы из образов" \
        "3" "Восстановить таблицу разделов (GPT)" \
        "4" "Выход" 3>&1 1>&2 2>&3)

    if [ $? -ne 0 ]; then
        printf '\033[H\033[J'; exit 0
    fi

    case "$menu" in
        "1") backup ;;
        "2") restore ;;
        "3") restore_gpt ;;
        "4") printf '\033[H\033[J'; exit 0 ;;
    esac

    printf "Press any key to continue..."
    read -s -n 1 dummy
    echo

done
