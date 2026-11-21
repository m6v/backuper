CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic

LDFLAGS_BACKUP  := -lext2fs -lcom_err
LDFLAGS_RESTORE := -lext2fs -lcom_err

TARGETS := pbackup prestore

.PHONY: build clean test

# Цель по умолчанию
build: $(TARGETS)

pbackup: pbackup.cpp format.hpp
	$(CXX) $(CXXFLAGS) pbackup.cpp -o $@ $(LDFLAGS_BACKUP)

prestore: prestore.cpp format.hpp
	$(CXX) $(CXXFLAGS) prestore.cpp -o $@ $(LDFLAGS_RESTORE)

clean:
	rm -f $(TARGETS) test_src.img test_dst.img test.img.zst

test: build
	@rm -f test_dst.img
	@echo "Создание тестового образа ext4 (100 МБ)..."
	dd if=/dev/zero of=test_src.img bs=1M count=100 status=none
	mkfs.ext4 -F -q test_src.img
	@echo "Клонирование через пайп в zstd..."
	./pbackup test_src.img - | zstd -f -q -o test.img.zst
	@echo "Восстановление в новый файл..."
	zstd -dc test.img.zst | ./prestore - test_dst.img
	@echo "Проверка целостности с помощью e2fsck..."
	e2fsck -fn test_dst.img
	@echo "Тест успешно пройден!"
