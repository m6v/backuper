#!/bin/bash

# Создаание базовой структуры каталогов
init_recovery_env() {
    local target_dir="$1"

    echo "Инициализация среды восстановления в $target_dir..."

    # Создание каталогов для виртуальных файловых систем
    mkdir -p "$target_dir"/{dev,etc,proc,run,sys,tmp,usr}

    # Установка правильных прав для /tmp
    chmod 1777 "$target_dir/tmp"

    # Создание вторичной иерархии внутри /usr
    mkdir -p "$target_dir/usr"/{bin,sbin,lib,lib/x86_64-linux-gnu,lib64}

    ln -sfn usr/bin  "$target_dir/bin"
    ln -sfn usr/sbin "$target_dir/sbin"
    ln -sfn usr/lib  "$target_dir/lib"
    ln -sfn usr/lib64 "$target_dir/lib64"
}

# Копирование исполняемого файла и его зависимостей
copy_bin_with_libs() {
    local input="$1"
    local target_dir="$2"
    local binary=""
    local interpreter=""

    # Поиск бинарника в пределах переменной PATH или проверка абсолютного пути
    if [[ "$input" == /* ]]; then
        binary="$input"
    else
        binary=$(PATH="$PATH:/usr/sbin:/sbin" which "$input" 2>/dev/null)
        if [ -z "$binary" ]; then
            binary=$(type -p "$input" 2>/dev/null)
        fi
    fi

    # Проверка существования файла
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        echo "[Ошибка] Программа '$input' не найдена" >&2
        return 1
    fi

    # Копирование исполного файла с сохранением путей и прав доступа (-p)
    echo "Копирование $binary..."
    cp --parents -p "$binary" "$target_dir/"

    # Извлечение путей к библиотекам и копирование библиотек с сохранением прав доступа (флаг -p)
    ldd "$binary" | awk '/\// {print $3}' | while read -r lib; do
        if [ -f "$lib" ]; then
            echo "Копирование $lib..."
            cp --parents -p "$lib" "$target_dir/"
        fi
    done

    # Копирование динамического загрузчика (интерпретатора ELF-файлов)
    interpreter=$(readelf -l "$binary" | grep -e "program interpreter" -e "интерпретатор программы" | tr -d '[]' | awk '{print $NF}')
    if [ -f "$interpreter" ]; then
        echo "Копирование $interpreter..."
        cp --parents -p "$interpreter" "$target_dir/"
    fi

    return 0
}

# Проверка, наличия не менее двух аргументов (программа и целевой каталог)
if [ "$#" -lt 2 ]; then
    echo "Использование: $0 <программа1> [программа2 ...] <целевой_каталог>"
    echo "Пример: $0 bash ls grep tar /tmp/recovery_root"
    exit 1
fi

# Извлечение последнего аргумента (целевого каталога)
TARGET_DIR="${@: -1}"

# Инициализация структуры каталогов
init_recovery_env "$TARGET_DIR"

# Цикл по всем аргументам-программам, кроме последнего
for cmd in "${@:1:$#-1}"; do
    copy_bin_with_libs "$cmd" "$TARGET_DIR"
    if [ $? -eq 0 ]; then
        echo "Компоненты $cmd скопированы"
    else
        echo "Ошибка при поиске и/или копировании компонентов для '$cmd'"
    fi
done

busybox_path=$(find "$TARGET_DIR" -name busybox -print -quit 2>/dev/null)
if [ -n $busybox_path ]; then
    echo "Инициализация окружения BusyBox..."
    # Создание относительных ссылок на busybox в одном каталоге с busybox
    for cmd in $($busybox_path --list); do
        ln -sf "busybox" "$(dirname $busybox_path)/$cmd"
    done
    echo "Копирование локалей..."
    cp --parents -p -r /usr/lib/locale "$TARGET_DIR"
fi

echo "Развертывание среды восстановления завершено"
