#!/bin/sh

BASE_DIR="$(cd $(dirname $0) && pwd)"
export PATH="$BASE_DIR:$PATH"
export LD_LIBRARY_PATH="$BASE_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"


select_disk_dialog() {
  items=$(fdisk -l | grep "^Disk /dev" | cut -d" " -f2 | sed "s/:.*/ -/")
  whiptail --title "Select disk" --menu "$1" --noitem 0 60 0 $items 3>&1 1>&2 2>&3
  return $?
}

# Функция для вывода диалога whiptail и возврата выбранных разделов
select_partitions_dialog() {
    local DISK="$1"
    
    # Быстрые проверки на входе
    [ -z "$DISK" ] && return 1
    local partitions_info=$(fdisk -l 2>/dev/null | grep "^$DISK" | tr -s " ")
    [ -z "$partitions_info" ] && return 1

    # Склеивание строк с разделителем '|' для обработки в цикле
    local lines_pipeline=$(echo "$partitions_info" | tr '\n' '|')

    # Очистка системных параметров для наполнения меню
    set -- 
    
    # Установка нового разделителя строк
    local IFS='|'
    for line in $lines_pipeline; do
        [ -z "$line" ] && continue

        # Извлекаем имя раздела (1-е поле) и описание (с 5-го поля)
        local partition=$(echo "$line" | cut -d" " -f1)
        local description=$(echo "$line" | cut -d" " -f5-)

        # Добавление элемента в массив для whiptail
        set -- "$@" "$partition" "$description" "OFF"
    done

    # Возврат стандартного разделителя строк
    unset IFS

    # Тихий выход, если массив пуст
    [ $# -eq 0 ] && return 1

    # Подсчет количества разделов (число аргументов деленное на 3)
    local count=$(( $# / 3 ))

    # Установка высоты внутренней рамки равной количеству разделов
    local list_height=$count
    
    # Установка высоты окна (количество строк + 7 строк на шапку и кнопки)
    local window_height=$(( count + 7 ))

    # Если разделов больше 10, включение ограничения и скроллбара
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
        $window_height 76 $list_height \
        --separate-output \
        "$@" 3>&1 1>&2 2>&3 </dev/tty)

    # Молчаливый возврат результата
    if [ $? -eq 0 ] && [ -n "$selections" ]; then
        echo "$selections"
        return 0
    else
        return 1
    fi
}

backup_gpt() {
  DISK=$1
  echo "Backup GPT of $DISK..."
  mkdir -p $BASE_DIR/$HOSTNAME
  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $DISK).sf

  if ! sfdisk -d $DISK > $IMAGE; then
    echo "Error. Failed to backup GPT"
    return 1
  fi
  echo "GPT has been successfully backed to $IMAGE"

  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $DISK).fd
  fdisk -l "$DISK" > $IMAGE
  echo "Save partitions info to $IMAGE"
}

restore_gpt() {
  DISK=$(select_disk_dialog)
  if [ $? -ne 0 ]; then
    echo "Warning. Restoring GUID partition table canceled"
    return 1;
  fi

  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $DISK).sf
  echo "Restoring GUID partition table of $DISK..."

  if mount | grep ^$DISK; then
    echo "Error. Device $DISK is busy"
    return 1
  fi
  if [ ! -f $IMAGE ]; then
    echo "Error. Dump of partition table $IMAGE is not found"
    return 2
  fi
  if sfdisk $DISK < $IMAGE; then
    echo "GPT has been successfully restored from $IMAGE"
   return 0
  fi
  echo "Error. Failed to restore GPT"
}

backup_part() {
  PARTITION=$1
  if mount | grep $PARTITION; then
    echo "Error. Partition $PARTITION is mounted, backup skipped"
    return 1
  fi

  mkdir -p $BASE_DIR/$HOSTNAME
  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $PARTITION).gz

  part_info=$(blkid | grep $PARTITION)
  part_type=$(echo "$part_info" | sed 's/.*TYPE="\([^"]*\).*/\1/')

  echo "Backup $PARTITION..."
  case "$part_type" in
    ext4|ext3|ext2)
      e2image -arpf $PARTITION - | gzip > $IMAGE
      ;;
    vfat)
      dd if=$PARTITION bs=4M status=progress | gzip > $IMAGE
      ;;
    *)
      echo "Error. Unsupported filesystem $part_type"
      ;;
  esac
}

restore_part() {
  PARTITION=$1
  if mount | grep $PARTITION; then
    echo "Partition $PARTITION is mounted, skipped"
    return 1
  fi

  IMAGE=$BASE_DIR/$HOSTNAME/$(basename $PARTITION).gz
  if [ ! -f $IMAGE ]; then
    echo "Error. Image $IMAGE is not found"
    return 2
  fi

  echo "Restoring partition $PARTITION..."
  # For backuping to real SSD/NVMe run "blkdiscard $PARTITION" before backuping and add "conv=sparse" option
  # For backuping to HDD add "conv=fdatasync" option
  gzip -dc $IMAGE | dd of=$PARTITION bs=4M status=progress
  # TODO Check PUUID and if is ESP, then restoring NVRAM
}

add_nvram_entry() {
  info=""
  # Iterate by all block devices
  for dev in /sys/class/block/*/device; do
    DISK=/dev/$(basename $(dirname $dev))
    info=$(sfdisk -d $DISK | grep C12A7328)
    if [ -n "$info" ]; then break; fi
  done
  if [ -z "$info" ]; then
    echo "Error. ESP is not found"
    return 1
  fi
  ESP_DISK="$DISK"
  # Get all sybmols before first space
  ESP_PART="${info%% *}"
  # Get all last digits
  ESP_NUM="${ESP_PART##*[a-zA-Z]}"
  # Get uuid
  t="${line#*uuid=}"
  ESP_UUID="${t%%,*}"

  echo "Add entry to NVRAM..."
  if ! efibootmgr -v | grep -iq $ESP_UUID; then
    echo "efibootmgr -c -d $ESP_DISK -p $ESP_NUM -L 'astralinux' -l '\EFI\astralinux\grubx64.efi'"
    echo "Boot entry successfully added to NVRAM"
  else
    echo "Warning. Boot entry already exists in NVRAM, skipped"
  fi
}

backup() {
  DISK=$(select_disk_dialog)
  if [ $? -ne 0 ]; then
    echo "Warning. Backuping partition(s) canceled"
    return 1
  fi

  PARTITIONS=$(select_partitions_dialog "$DISK")
  # Проверка, что выбран хотя бы один раздел
  if [ -n "$PARTITIONS" ]; then

    whiptail --title "Backuping selected partition(s)" --infobox "Please wait. Progress info in the status line" 7 50
    backup_gpt  "$DISK"
    # Передача списка строк в цикл построчно
    echo "$PARTITIONS" | while IFS= read -r PART; do
      # Пропуск пустых строк
      [ -z "$PART" ] && continue
      backup_part "$PART"
    done

  else
    echo "Warning. Список разделов пуст"
  fi
}

restore() {
  DISK=$(select_disk_dialog)
  if [ $? -ne 0 ]; then
    echo "Warning. Restoring partition(s) canceled"
    return 1
  fi

  PARTITIONS=$(select_partitions_dialog "$DISK")
  # Проверка, что выбран хотя бы один раздел
  if [ -n "$PARTITIONS" ]; then
    whiptail --title "Restoring selected partition(s)" --infobox "Please wait. Progress info in the status line" 7 50
    # Передача списка строк в цикл построчно
    echo "$PARTITIONS" | while IFS= read -r part; do
      # Пропуск пустых строк
      [ -z "$part" ] && continue
      restore_part "$part"
    done
  else
    echo "Warning. Список разделов пуст"
  fi
}

while true; do
  HOSTNAME=$(whiptail --title "Hostname Configuration" --inputbox "Enter the hostname:" 10 50 "$(hostname)" 3>&1 1>&2 2>&3)
  if [ $? -ne 0 ]; then exit 1; fi

  if echo $HOSTNAME | grep -qxE '^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$'; then
    break
  fi
  whiptail --title "Error" --msgbox "Invalid hostname '$HOSTNAME'. Please try again" 0 50
done

  while true; do
    menu=$(whiptail --title "Main menu" --menu "Select action for $HOSTNAME:" 0 60 0 \
    "1" "Backup partition(s)" \
    "2" "Restore partition(s)" \
    "3" "Restore GUID partition table" \
    "4" "Add NVRAM entry" \
    "5" "Exit" 3>&1 1>&2 2>&3)

  if [ $? -ne 0 ]; then
    printf '\033[H\033[J'; exit 0
  fi

  case "$menu" in
    "1") backup ;;
    "2") restore ;;
    "3") restore_gpt ;;
    "4") add_nvram_entry ;;
    "5") printf '\033[H\033[J'; exit 0 ;;
  esac

  printf "Press any key to continue..."
  read -s -n 1 dummy

done
