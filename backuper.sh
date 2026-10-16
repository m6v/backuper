#!/bin/sh

BASE_DIR="$(cd $(dirname $0) && pwd)"
export PATH="$BASE_DIR:$PATH"
export LD_LIBRARY_PATH="$BASE_DIR/lib:${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

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

# Диалог выбора дисков
select_disk_dialog() {
  items=$(fdisk -l | grep -E "^(Disk|Диск) /dev" | cut -d" " -f2 | sed "s/:.*/ -/")
  whiptail --title "Выбор диска" --menu "$1" --noitem 0 60 0 $items 3>&1 1>&2 2>&3
  return $?
}

# Диалог выбора разделов на диске
select_partitions_dialog() {
    local DISK="$1"
    
    # Быстрые проверки на входе
    [ -z "$DISK" ] && return 1

    local partitions_info=$(fdisk -l 2>/dev/null | grep "^$DISK" | tr -s " ")
    [ -z "$partitions_info" ] && return 1

    # Склеивание строк с разделителем '|' для обработки в цикле
    local lines_pipeline=$(echo "$partitions_info" | tr '\n' '|')

    # Очистка позиционных параметров, переданных скрипту при запуске
    set -- 

    # Установка нового разделителя строк
    local IFS='|'
    # Счетчик разделов
    local part_count=0
    for line in $lines_pipeline; do
        [ -z "$line" ] && continue
        # Извлечение имени раздела (1-е поле) и описания (с 5-го поля до конца)
        local partition=$(echo "$line" | cut -d" " -f1)
        local description=$(echo "$line" | cut -d" " -f5-)
        # Добавление нового триплета к списку параметров для последующей передачи whiptail
        set -- "$@" "$partition" "$description" "ON"
        part_count=$(( part_count + 1 ))
    done

    # Возврат стандартного разделителя строк
    unset IFS

    # Выход, если на диске нет разделов
    if [ $part_count -eq 0 ]; then
      echo "No partitions found on $DISK. Please restore GPT from dump"
      #TODO Добавить вывод диалога с предложением восстановить GPT (если файл $DISK.sf в наличии)
      return 1
    fi
    
    # Установка высоты внутренней рамки равной количеству разделов
    local list_height=$part_count
    
    # Установка высоты окна (количество строк + 7 строк на заголовок и кнопки)
    local window_height=$(( part_count + 7 ))

    # Ограничение высоты списка и окна значениями 10 и 17 соответственно
    if [ "$list_height" -gt 10 ]; then
        list_height=10
        window_height=17
    fi

    # Изменение цветов элементов списка
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
        "$@" 3>&1 1>&2 2>&3 </dev/tty)

    # Возврат списка выбранных разделов, если нажата <OK>
    if [ $? -eq 0 ] && [ -n "$selections" ]; then
        echo "$selections"
        return 0
    else
        return 1
    fi
}

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
    # Это SSD диск
    echo "Discard blocks on $PARTITION"
    run 'blkdiscard $PARTITION'
    parm=""
  else
    # Это HDD диск
    echo "Wipe signatures on $PARTITION"
    run 'wipefs -a "$PARTITION"'
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
    echo "Operation cancelled"
    return 1
  fi

  # Проверка, что выбран хотя бы один раздел
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
    echo "Operation canceled"
    return 1
  fi
  
  # Проверка, что выбран хотя бы один раздел
  if [ -z "$PARTITIONS" ]; then
    echo "No partitions selected. Operation cancelled"
    return 1
  fi

  whiptail --title "Восстановление разделов из образов" --infobox "Ждите завершения операции. Прогресс в строке состояния" 7 60
  # Передача списка строк в цикл построчно
  echo "$PARTITIONS" | while IFS= read -r PART; do
    restore_part $DISK $PART
  done
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
STORAGE=$BASE_DIR/$HOSTNAME

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

done
