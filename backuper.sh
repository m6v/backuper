#!/bin/sh

# Настройка переменных
BASE_DIR="$(cd $(dirname $0) && pwd)"
export PATH="$BASE_DIR:$PATH"
export LD_LIBRARY_PATH="$BASE_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

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

    # Выход, если массив пуст (на диск не размечен)
    [ $# -eq 0 ] && return 1

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
        --checklist "Выберите разделы для работы (Пробел - выбор, Enter - ОК):" \
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
  echo "Backuping GPT of $DISK..."
  mkdir -p $BASE_DIR/$HOSTNAME
  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $DISK).sf

  if ! sfdisk -d $DISK > $IMAGE; then
    echo "Failed to backup GPT"
    return 1
  fi
  echo "GPT has been successfully backed to $IMAGE"

  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $DISK).fd
  fdisk -l "$DISK" > $IMAGE
  echo "Partitions info saved to $IMAGE"
}

restore_gpt() {
  DISK=$(select_disk_dialog)
  if [ $? -ne 0 ]; then
    echo "Restoring GUID partition table canceled"
    return 1;
  fi

  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $DISK).sf
  echo "Restoring GUID partition table of $DISK..."

  if mount | grep ^$DISK; then
    echo "Device $DISK is busy"
    return 1
  fi
  if [ ! -f $IMAGE ]; then
    echo "Dump of partition table $IMAGE is not found"
    return 2
  fi
  if sfdisk $DISK < $IMAGE; then
    echo "GPT has been successfully restored from $IMAGE"
   return 0
  fi
  echo "Failed to restore GPT"
}

backup_part() {
  DISK=$1
  PARTITION=$2
  if mount | grep $PARTITION; then
    echo "Partition $PARTITION is mounted, backup skipped"
    return 1
  fi

  mkdir -p $BASE_DIR/$HOSTNAME
  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $PARTITION).gz

  part_info=$(blkid | grep $PARTITION)
  part_type=$(echo "$part_info" | sed 's/.*TYPE="\([^"]*\).*/\1/')

  echo "Backuping $PARTITION..."
  case "$part_type" in
    ext4|ext3|ext2)
      e2image -arpf $PARTITION - | gzip > $IMAGE
      ;;
    swap)
      echo "Skipped swap partition"
      ;;
    *)
      dd if=$PARTITION bs=4M status=progress | gzip > $IMAGE
      ;;
  esac
}

restore_part() {
  DISK=$1
  PARTITION=$2
  if mount | grep $PARTITION; then
    echo "Partition $PARTITION is mounted, skipped"
    return 1
  fi

  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $PARTITION).gz
  if [ ! -f $IMAGE ]; then
    echo "Image $IMAGE is not found"
    return 2
  fi

  # Для свопа выполнение форматирования вместо копировния
  part_info=$(blkid | grep $PARTITION)
  part_type=$(echo "$part_info" | sed 's/.*TYPE="\([^"]*\).*/\1/')
  if [ part_type = "swap" ]; then
    echo "Formating swap partition $PARTITION..."
    mkswap $PARTITION
    return 0
  fi

  echo "Restoring partition $PARTITION..."
  # Определение типа диска и установка доп. параметра
  if [ $(cat /sys/block/$(basename $DISK)/queue/rotational) -eq 0 ]; then
    # Это SSD диск
    blkdiscard $PARTITION
    parm="conv=sparse"
  else
    # Это HDD диск
    parm="conv=fdatasync"
  fi
  gzip -dc $IMAGE | dd of=$PARTITION bs=4M status=progress $parm
  
  # Добавление записи в NVRAM, если восстанавливаемый раздел это ESP
  PARTITION_INFO=$(sfdisk -d $DISK | grep $PARTITION.*C12A7328)
  if [ -z "$PARTITION_INFO" ]; then
    return 0
  fi

  PARTITION_NUMBER="${PARTITION##*[a-zA-Z]}"
  PARTITION_UUID="${PARTITION_INFO#*uuid=}"
  echo "Adding boot entry to NVRAM..."
  if ! efibootmgr -v | grep -iq $PARTITION_UUID; then
    efibootmgr -c -d $DISK -p $PARTITION_NUMBER -L 'astralinux' -l '\EFI\astralinux\grubx64.efi'
    echo "Boot entry added to NVRAM succefully"
  else
    echo "Boot entry already exists in NVRAM, skipped"
  fi
}

backup() {
  DISK=$(select_disk_dialog)
  if [ $? -ne 0 ]; then
    echo "Backuping partition(s) canceled"
    return 1
  fi

  PARTITIONS=$(select_partitions_dialog "$DISK")
  # Проверка, что выбран хотя бы один раздел
  if [ -n "$PARTITIONS" ]; then

    whiptail --title "Создание образов разделов" --infobox "Ждите завершения операции. Прогресс в строке состояния" 7 60
    backup_gpt  "$DISK"
    # Передача списка строк в цикл построчно
    echo "$PARTITIONS" | while IFS= read -r PART; do
      # Пропуск пустых строк
      [ -z "$PART" ] && continue
      backup_part $DISK $PART
    done

  else
    echo "No partitions selected"
  fi
}

restore() {
  DISK=$(select_disk_dialog)
  if [ $? -ne 0 ]; then
    echo "Restoring partition(s) canceled"
    return 1
  fi

  PARTITIONS=$(select_partitions_dialog "$DISK")
  # Проверка, что выбран хотя бы один раздел
  if [ -n "$PARTITIONS" ]; then
    whiptail --title "Восстановление разделов из образов" --infobox "Ждите завершения операции. Прогресс в строке состояния" 7 60
    # Передача списка строк в цикл построчно
    echo "$PARTITIONS" | while IFS= read -r PART; do
      # Пропуск пустых строк
      [ -z "$PART" ] && continue
      restore_part $DISK $PART
    done
  else
    echo "No partitions selected"
  fi
}

while true; do
  HOSTNAME=$(whiptail --title "Ввод имени хоста" --inputbox "Введите имя хоста:" 10 60 "$(hostname)" 3>&1 1>&2 2>&3)
  if [ $? -ne 0 ]; then exit 1; fi

  if echo $HOSTNAME | grep -qxE '^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$'; then
    break
  fi
  whiptail --title "Error" --msgbox "Недопустимое имя хоста '$HOSTNAME'. Повторите ввод" 0 60

done

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
