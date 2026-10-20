# Названия исходного скрипта и итогового бинарника
SOURCE = backuper.sh
TARGET = backuper

# Компилятор и утилиты
CC = gcc
OBJCOPY = objcopy

# Временные файлы сборки
ARCHIVE = /tmp/payload.tar.gz
OBJ = payload.o

# Цель по умолчанию
all: $(TARGET)

# Сборка финального бинарника
$(TARGET): main.c $(OBJ)
	@echo "Компиляция финального бинарника $(TARGET)..."
	$(CC) main.c $(OBJ) -o $(TARGET)
	@echo "Сборка успешно завершена"
	@$(MAKE) clean_temp

# Превращение архива в объектный файл
$(OBJ): $(ARCHIVE)
	@echo "Трансляция архива в объектный модуль ELF64..."
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386 $(ARCHIVE) $(OBJ)

# Упаковка во временную директорию
$(ARCHIVE): $(SOURCE)
	@echo "Упаковка скрипта и зависимостей в $(ARCHIVE)..."
	tar -czf archive.tar.gz */

# Очистка проекта
clean:
	@echo "Очистка временных файлов и бинарника $(TARGET)..."
	rm -f $(ARCHIVE) $(OBJ) $(TARGET)

.PHONY: all clean clean_temp
