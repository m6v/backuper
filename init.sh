#!/bin/bash

# Создаание базовой структуры каталогов
init_recovery_env() {
    local target_dir="$1"

    echo "Инициализация среды восстановления в $target_dir..."

    # Создание каталогов для виртуальных файловых систем
    mkdir -p "$target_dir"/{dev,proc,sys}

    # Установка правильных прав для /tmp
    # chmod 1777 "$target_dir/tmp"
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
        binary=$(which "$input" 2>/dev/null)
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

    # Извлечение путей к библиотекам и копирование библиотек
    ldd "$binary" | awk '/\// {print $3}' | while read -r lib; do
        if [ -f "$lib" ]; then
            echo "Копирование $lib..."
            cp --parents -p "$lib" "$target_dir/"
        fi
    done

    # Обрабатываем динамический загрузчик (интерпретатор ELF-файлов) через readelf
    interpreter=$(readelf -l "$binary" | grep 'program interpreter' | tr -d '[]' | awk '{print $NF}')
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
        echo "Компоненты для '$cmd' скопированы"
    else
        echo "Ошибка при поиске и/или копировании компонентов для '$cmd'"
    fi
done

echo "Развертывание среды восстановления успешно завершено!"
