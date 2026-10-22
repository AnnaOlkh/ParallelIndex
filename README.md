# Паралельна побудова зворотного індексу

Програма будує зворотний індекс текстових документів UTF-8. Містить версії V0–V4 (`indexer`) і окрему програму дослідження хибного розділення (`false_sharing`).

## Залежності

- CMake 3.20+ (для presets — 3.21+).
- Компілятор C++20 із підтримкою `std::jthread` і `std::barrier`. Перевірено: MSYS2 UCRT64, GCC 13.2.0, MinGW Make.
- PowerShell (сценарій вимірювань) і Python 3.10+ (аналіз CSV, лише стандартна бібліотека).

Сторонніх бібліотек немає, таблиці Unicode 15.0.0 входять до коду. Python потрібен лише для аналізу результатів.

## Збирання (Windows, з кореня проєкту)

`C:/msys64/ucrt64/bin` має бути в `PATH`; якщо MSYS2 встановлено деінде, змініть шляхи.

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/mingw32-make.exe `
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" `
  -DENABLE_UNSAFE_V1=OFF `
  -DBUILD_TESTING=ON

cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Результат: `build/indexer.exe`, `build/false_sharing.exe`, `build/index_tests.exe`.

Для режиму V1 unsafe (гонка даних, невизначена поведінка за C++) потрібна окрема збірка: ті самі команди з `-B build-unsafe` і `-DENABLE_UNSAFE_V1=ON`.

Те саме через presets: `cmake --preset mingw-release` (або `mingw-unsafe`), далі `cmake --build --preset ...` і `ctest --preset ...`.

### Параметри компілятора

| Параметр | Призначення |
|---|---|
| `-std=c++20` | Стандарт C++20 без розширень GNU |
| `-O3 -DNDEBUG` | Оптимізація Release |
| `-Wall -Wextra -Wpedantic` | Попередження |
| `-DLAB_UNSAFE_V1=0` / `=1` | Доступність V1 unsafe в бібліотеці індексатора |
| `-I<каталог>/src` | Шлях до заголовків |
| `-DFIXTURE_DIR=\"<каталог>/tests/fixtures\"` | Тестові документи (лише `index_tests`) |

Потоки підключаються через `Threads::Threads`. Повні команди компіляції: `cmake --build build --clean-first --verbose` або `compile_commands.json` у каталозі збирання.

## Вхід і вихід

- Вхід: каталог із текстами UTF-8; файли обробляються рекурсивно незалежно від розширення, символічні посилання пропускаються, некоректний UTF-8 дає помилку.
- ID документів: від 1, за відсортованими відносними шляхами. V0 також створює `<output>.documents.tsv` (ID і шляхи).
- Нормалізація: нижній регістр; роздільник — усе, крім літер Unicode та десяткових цифр; токени коротші за 3 кодові точки відкидаються.
- Вихідний файл: UTF-8 без BOM, рядки закінчуються LF.

```text
apple<TAB>3<TAB>doc1:2,doc2:1
```

Поля: слово, загальна кількість, входження за документами. Слова впорядковано за байтами UTF-8; документи — за спаданням кількості, за рівності — за зростанням числового ID.

Вихідний файл, еталон і CSV мають бути різними файлами поза вхідним каталогом.

## Запуск `indexer`

Приклади на малому корпусі `tests/fixtures` (еталон V0 потрібен для `--verify`):

```powershell
./build/indexer.exe --input tests/fixtures --output results/v0.tsv --version v0
./build/indexer.exe --input tests/fixtures --output results/v1-defined.tsv --version v1 --v1-mode defined --threads 4 --verify results/v0.tsv
./build-unsafe/indexer.exe --input tests/fixtures --output results/v1-unsafe.tsv --version v1 --v1-mode unsafe --threads 4 --verify results/v0.tsv
./build/indexer.exe --input tests/fixtures --output results/v2.tsv --version v2 --threads 4 --verify results/v0.tsv
./build/indexer.exe --input tests/fixtures --output results/v3.tsv --version v3 --threads 4 --shards 64 --verify results/v0.tsv
./build/indexer.exe --input tests/fixtures --output results/v4-sequential.tsv --version v4 --threads 4 --merge sequential --verify results/v0.tsv
./build/indexer.exe --input tests/fixtures --output results/v4-tree.tsv --version v4 --threads 4 --merge tree --verify results/v0.tsv
```

V0, V2, V3, V4 дають однаковий файл. У V1 розбіжність з еталоном лише записується в результат перевірки, програма не завершується аварійно: `defined` використовує окремі атомарні читання й запис (не атомарний інкремент), `unsafe` — звичайні.

### Аргументи

| Аргумент | За замовчуванням | Опис |
|---|---|---|
| `--input DIR` | обов’язковий | Каталог документів |
| `--output FILE` | обов’язковий | Файл індексу |
| `--version v0\|v1\|v2\|v3\|v4` | `v0` | Версія |
| `--threads N` | `1` | Потоки, 1–4096 (для V0 лише 1) |
| `--shards K` | `64` | Секції V3, 1–65536 |
| `--merge sequential\|tree` | `sequential` | Злиття V4 |
| `--v1-mode defined\|unsafe` | `defined` | Режим V1; `unsafe` потребує збірки `build-unsafe` |
| `--verify FILE` | — | Побайтове порівняння з еталоном V0 |
| `--repeats N` | `1` | Вимірювані повторення |
| `--warmup N` | `1` | Прогріви поза вимірюваннями, не менше 1 |
| `--csv FILE` | — | Дописати вимірювання в CSV (для порожнього файлу створюється заголовок) |
| `--require-corpus yes` | — | Вимагати ≥200 файлів і ≥200 МіБ |
| `--experiment index\|scaling\|v1_repro\|v3_sweep` | `index` | Група вимірювань у CSV |
| `--repeat-start N` | `1` | Початковий номер повторення в CSV |
| `--help` | — | Довідка |
| `--info` | — | Компілятор, стандарт, Unicode, конфігурація (запускається окремо) |

`--repeats`, `--warmup`, `--repeat-start` — цілі від 1 до 1 000 000. Помилки аргументів і читання, а також розбіжність коректної версії з еталоном дають ненульовий код завершення.

## Хибне розділення

```powershell
./build/false_sharing.exe --repeats 5 --iterations 10000000 --csv results/false-sharing.csv
./build/false_sharing.exe --info
```

Для P = 1, 2, 4, 8 вимірюються два розміщення лічильників: A (`adjacent`) — сусідні, B (`padded`) — кожен в окремому рядку кешу. Перед кожною точкою один прогрів; після потоків перевіряються кінцеві значення.

| Аргумент | За замовчуванням | Опис |
|---|---|---|
| `--repeats N` | `5` | Вимірювані повторення |
| `--iterations N` | `10000000` | Інкрементів на потік |
| `--cache-line N` | з ОС | Розмір рядка кешу, байти: степінь двійки, не більше 4096 і не менше `alignof(std::max_align_t)` |
| `--csv FILE` | — | Файл вимірювань |
| `--help`, `--info` | — | Довідка; відомості про кеші й пам’ять |

## Серія вимірювань і аналіз

Корпус не входить до коду. Покладіть його в `corpus/pg19` (PG-19: https://github.com/google-deepmind/pg19) або вкажіть шлях через `-Corpus`. Перед запуском закрийте сторонні програми, вимкніть енергозбереження, використовуйте Release-збірку.

```powershell
./scripts/run_experiments.ps1 -Corpus corpus/pg19 -BuildDir build-unsafe -Threads 1,2,4,8 -FixedP 8 -ScalingK 64 -Repeats 5 -V1Mode unsafe
python scripts/analyze.py results/run-<дата-час>/results.csv --scaling-k 64 --fixed-p 8
```

У другій команді підставте назву каталогу, створеного першою. Сценарій створює `results/run-<дата-час>/` і виконує: V0; десять запусків V1 при `-FixedP`; V1–V4 при P = 1, 2, 4, 8 (обидва способи злиття V4); V3 при K = 1, 4, 16, 64, 256, 1024, 4096; обидва розміщення хибного розділення.

| Параметр | Значення |
|---|---|
| `-Repeats` | Повторення кожної основної точки, не менше 5 |
| `-FixedP` | Потоки для підбору K і десяти запусків V1 |
| `-ScalingK` | K для залежності від P |
| `-BuildDir`, `-V1Mode` | Каталог збірки; режим V1 |
| `-Demo` | Не перевіряти обсяг корпусу (для малих даних) |

`analyze.py` формує таблицю медіан і min–max, таблицю втрат V1 та SVG-графіки (прискорення, ефективність, залежність від K, хибне розділення, злиття). `S(P)=T(V0)/T(P)`, `E(P)=S(P)/P`.

### Поля `results.csv`

| Поле | Опис |
|---|---|
| `experiment`, `version` | Група вимірювань, версія |
| `threads`, `K`, `merge`, `v1_mode` | Потоки, секції, злиття, режим V1 |
| `repeat` | Номер повторення |
| `build_ms` | Побудова індексу (з підготовкою V1 і збиранням секцій V3) |
| `merge_ms` | Злиття V4, для інших версій 0 |
| `canonicalize_ms` | Сортування й форматування в пам’яті |
| `compute_ms` | `build_ms + merge_ms + canonicalize_ms`; для хибного розділення — цикли зі стартовим бар’єром і приєднанням потоків (створення потоків не входить) |
| `read_ms`, `write_ms` | Читання корпусу й запис індексу; не входять до `compute_ms` |
| `total_occurrences` | Сума входжень токенів (для хибного розділення — сума інкрементів) |
| `correct` | 1 — збіг з еталоном, 0 — розбіжність |
| `files`, `bytes` | Кількість документів і обсяг корпусу |

Корпус читається один раз на процес, тому `read_ms` повторюється в рядках запуску. Кожне повторення будує індекс заново; перевірки, запис CSV і підготовка еталона до часу обчислення не входять.