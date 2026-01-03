# План подготовки проекта TaskbarWidgetPDH к публикации в Git

## Необходимые файлы

1. **.gitignore** - игнорирование временных файлов сборки
2. **README.md** - описание проекта, инструкции по сборке и использованию
3. **LICENSE** - лицензия MIT
4. **CHANGELOG.md** - история изменений (опционально)

## Содержание файлов

### .gitignore
```
# Compiled binaries
*.exe
*.obj
*.res
*.pdb
*.ilk
*.lib
*.exp

# Build directories
build/
x64/
Debug/
Release/

# IDE files
.vs/
*.vcxproj
*.vcxproj.filters
*.sln
*.suo
*.user

# OS generated files
.DS_Store
Thumbs.db

# Temporary files
*.tmp
*.log
```

### README.md
- Название проекта
- Краткое описание (виджет для отображения метрик в панели задач)
- Требования (Windows, Visual Studio)
- Инструкция по сборке (запуск build.bat)
- Использование
- Скриншоты (если есть)
- Лицензия

### LICENSE
- Стандартная лицензия MIT

### CHANGELOG.md
- Версия 1.0.0: Первоначальный релиз
- Основные функции