#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <sys/wait.h>

// Константа с именем главного скрипта внутри архива
#define SCRIPT_NAME "backuper.sh"

// Элементы, сгенерированные утилитой objcopy из файла payload.tar.gz
extern char _binary__tmp_payload_tar_gz_start;
extern char _binary__tmp_payload_tar_gz_end;

int main(int argc, char *argv[]) {
    // Получение текущего каталога запуска, чтобы передать его скрипту
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        setenv("CURRENT_PWD", cwd, 1);
    }

    // Создание шаблона для временной папки в /tmp
    char tmp_dir[] = "/tmp/sfx_c_XXXXXX";
    if (mkdtemp(tmp_dir) == NULL) {
        perror("[ERROR] Не удалось создать временную директорию");
        return 1;
    }

    // Путь к временному файлу архива
    char tar_path[PATH_MAX];
    snprintf(tar_path, sizeof(tar_path), "%s/payload.tar.gz", tmp_dir);

    // Запись архива из памяти бинарника во временный каталог
    FILE *fp = fopen(tar_path, "wb");
    if (!fp) {
        perror("[ERROR] Не удалось записать временный архив");
        return 1;
    }

    // Вычисление размера приатаченного архива
    size_t payload_size = &_binary__tmp_payload_tar_gz_end - &_binary__tmp_payload_tar_gz_start;
    fwrite(&_binary__tmp_payload_tar_gz_start, 1, payload_size, fp);
    fclose(fp);

    // Рапасковка созданного tar.gz файла во временном каталоге tmp_dir
    char cmd_unpack[PATH_MAX * 2 + 20];
    snprintf(cmd_unpack, sizeof(cmd_unpack), "tar -xzf %s -C %s", tar_path, tmp_dir);
    if (system(cmd_unpack) != 0) {
        fprintf(stderr, "[ERROR] Ошибка распаковки архива.\n");
        return 1;
    }
    
    // Удаление архива .tar.gz из временного каталога
    unlink(tar_path);

    // Запуск скрипта  сохранение кода его завершения (exit code)
    char cmd_run[PATH_MAX + 20];
    snprintf(cmd_run, sizeof(cmd_run), "/bin/bash %s/%s", tmp_dir, SCRIPT_NAME);
    int script_status = system(cmd_run);
    int exit_code = WEXITSTATUS(script_status);

    // Полная очистка временного каталога
    char cmd_clean[PATH_MAX + 20];
    snprintf(cmd_clean, sizeof(cmd_clean), "rm -rf %s", tmp_dir);
    system(cmd_clean);

    // Возвращение тот же кода, который вернул скрипт
    return exit_code;
}
