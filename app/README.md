# Аналог partclone.extfs

Бэкап файла или раздела с образом
```bash
/home/m6v/Workspace/backuper/app/pbackup src.img - --progress-fd 3 3> >(whiptail --title "Backup" --gauge "Создание резервной копии..." 8 60 0 > /dev/tty) | zstd -q -T0 -f -o backup.img.zst
```

Восстановление файла или раздела из образа
```bash
zstd -q -dc backup.img.zst | /home/m6v/Workspace/backuper/app/prestore - dst.img --progress-fd 3 3> >(whiptail --title "Restore" --gauge "Восстановление раздела..." 8 60 0 > /dev/tty)
```
