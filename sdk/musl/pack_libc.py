import os
import sys
import subprocess

ar_bin = "x86_64-elf-ar"
output_lib = "lib/libc.a"
obj_dir = "obj/src"

# Создаем папку lib, если её нет
os.makedirs("lib", exist_ok=True)

if os.path.exists(output_lib):
    os.remove(output_lib)

# Собираем все скомпилированные .o файлы из obj/src
obj_files = []
for root, _, files in os.walk(obj_dir):
    for file in files:
        if file.endswith(".o"):
            obj_files.append(os.path.join(root, file))

print(f"[PACK] Найдено {len(obj_files)} объектных файлов. Упаковываем в {output_lib}...")

# Упаковываем частями по 50 файлов, чтобы не превышать лимит Windows
batch_size = 50
for i in range(0, len(obj_files), batch_size):
    batch = obj_files[i:i + batch_size]
    subprocess.run([ar_bin, "q", output_lib] + batch, check=True)

print(f"[SUCCESS] Библиотека {output_lib} успешно создана!")
