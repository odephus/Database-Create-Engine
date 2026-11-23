# Database Create Engine (DBCE)

DBCE is an embedded key–value store written in C11. It stores each database in a single append-only `.dbce` file. Use the CLI for text records or the C API for binary values.

[English](#english) · [Türkçe](#türkçe)

## English

### Build and test

Run these commands from the repository root. The examples use CMake 3.20 or later. On Windows, put MinGW-w64 GCC with POSIX threads and `mingw32-make` on your `PATH`.

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc
cmake --build build
ctest --test-dir build --output-on-failure
```

On Linux, install a C11 compiler, POSIX threads, CMake and Make, then run:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests use temporary database files. For sanitizer testing on Linux, configure separate build directories with `-DCMAKE_C_FLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"` and `-DCMAKE_C_FLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=thread"`.

### Create a database and use it

This PowerShell session creates `notes.dbce` in the current directory. Running `add` again with the same key replaces its value. On Linux, use `./build/dbce` in place of `.\build\dbce.exe`.

```powershell
.\build\dbce.exe create db notes
.\build\dbce.exe add notes title "First draft"
.\build\dbce.exe get notes title
.\build\dbce.exe get notes table
.\build\dbce.exe db list
.\build\dbce.exe go notes
```

`get notes title` displays a row containing `title` and `First draft`. The last command opens the interactive shell. Enter the following commands at its prompt:

```text
add title Revised draft
get title
del title
get table
exit
```

After the deletion, `get table` shows zero active records in this example. For a single deletion from your terminal, use `dbce del notes title`. Use `help` in either mode to list commands.

### Use the C API

Include [dbce.h](dbce.h) and link the CMake target `dbce_engine`. Open a file with `dbce_open("notes.dbce")`. To use the line-based settings in [config.json](config.json), call `dbce_open_config("config.json")`. It reads `db_path` and `queue_size`. The CLI takes its database name from the command line and reads `lang` from the current directory’s config file for help and shell messages (`en` or `tr`).

| Operation | What the caller can rely on |
| --- | --- |
| `dbce_set`, `dbce_del` | Success means the worker wrote the complete record, flushed the stream, and updated the index. Deleting an absent key also writes a tombstone. |
| `dbce_set_async` | Copies the input and returns when the queue accepts it. A full queue returns `false` with `errno == EAGAIN`. |
| `dbce_sync_wait_checked` | Waits for writes submitted through the start of the call. It reports the first accepted write that failed in that range since open. Later barriers continue to report it. |
| `dbce_get` | Returns a caller-owned buffer and its byte length. Read binary data using that length and release the buffer with `free()`. A missing key sets `errno` to `ENOENT`. |
| `dbce_scan_all_checked` | Visits a snapshot. Callbacks can read or write through the same handle. A scan can report a read or allocation error after earlier callbacks have run. |
| `dbce_close_checked` | Drains accepted writes, joins the worker, and reports write or close errors. Finish other calls and callbacks before closing. |

Use one open handle per file across all processes. Threads can share that handle. The queue mutex protects admission and completion. A separate mutex protects the file and index. The worker releases the storage mutex before announcing completion, and scans invoke callbacks with both mutexes released. Failed calls returning a pointer or boolean set `errno` on the calling thread.

### File limits and recovery

- Keys are C strings of 1–63 bytes. Values contain at least one byte and must fit both a `uint32_t` length and a `size_t` allocation with room for a trailing NUL on read. A zero length on disk denotes deletion.
- The index holds 4096 distinct keys, including deleted keys. At capacity, existing keys can still be updated. Adding another distinct key fails with `ENOSPC`.
- The legacy header contains four native-endian 32-bit fields in order: magic, reserved CRC, key length, value length. Raw key bytes (without a NUL) and value bytes follow. New records retain a zero CRC. Compatible legacy files open without migration.
- Open validates record boundaries. A malformed file or incomplete tail fails with `EILSEQ`; the file stays intact. Preserve a copy for manual recovery or restore a known-good backup. An append I/O error stops further writes on that handle.
- A successful write flushes data to the operating system. Power loss can still lose acknowledged data. The format has no verified checksum or commit marker, so a same-length corruption can go undetected and a complete record from a failed write can appear after reopening.

## Türkçe

DBCE, C11 ile yazılmış, uygulamaya gömülebilen bir anahtar-değer veritabanıdır. Her veritabanı tek bir `.dbce` dosyasında tutulur. Güncellemeler ve silme kayıtları dosyanın sonuna eklenir. Metin değerleri için komut satırını, ikili veriler için C API’sini kullanabilirsiniz.

### Derleme ve test

Komutları depo kök dizininde çalıştırın. Örnekler CMake 3.20 veya üzerini kullanır. Windows’ta POSIX iş parçacıklarını destekleyen MinGW-w64 GCC ile `mingw32-make` komutunun `PATH` üzerinde bulunması gerekir.

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc
cmake --build build
ctest --test-dir build --output-on-failure
```

Linux’ta C11 derleyicisi, POSIX iş parçacıkları, CMake ve Make gerekir.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Testler geçici veritabanı dosyaları kullanır. Linux’ta bellek hataları ve veri yarışları için ayrı derleme dizinlerinde çalıştırılacak derleyici seçenekleri [İngilizce derleme bölümünde](#build-and-test) verilmiştir.

### Veritabanı oluşturma ve kullanma

Bu PowerShell örneği, bulunduğunuz dizinde `notlar.dbce` dosyasını oluşturur. Aynı anahtarla tekrar `add` çalıştırırsanız değer güncellenir. Linux’ta `.\build\dbce.exe` yerine `./build/dbce` kullanın.

```powershell
.\build\dbce.exe create db notlar
.\build\dbce.exe add notlar baslik "Ilk taslak"
.\build\dbce.exe get notlar baslik
.\build\dbce.exe get notlar table
.\build\dbce.exe db list
.\build\dbce.exe go notlar
```

`get notlar baslik` çıktısında `baslik` anahtarını ve `Ilk taslak` değerini görürsünüz. Son komut etkileşimli kabuğu açar. Aşağıdaki komutları bu kabukta çalıştırın:

```text
add baslik Son taslak
get baslik
del baslik
get table
exit
```

Bu örnekte silme işleminden sonra `get table` sıfır etkin kayıt gösterir. Aynı silme işlemini doğrudan terminalden yapmak için `dbce del notlar baslik` kullanılır. Her iki kipte de `help` komutları listeler.

### C API’sini kullanma

[dbce.h](dbce.h) başlığını ekleyin ve uygulamanızı CMake’deki `dbce_engine` hedefine bağlayın. Dosyayı doğrudan açmak için `dbce_open("notlar.dbce")`, [config.json](config.json) içindeki `db_path` ve `queue_size` alanlarını kullanmak için `dbce_open_config("config.json")` çağrılır. CLI, veritabanı adını komut satırından alır. Yardım ve kabuk iletilerinin dili, çalışma dizinindeki yapılandırma dosyasının `lang` alanından seçilir (`en` veya `tr`).

| İşlem | Davranış |
| --- | --- |
| `dbce_set`, `dbce_del` | Kayıt tamamen yazıldıktan, akış işletim sistemine aktarıldıktan ve indeks güncellendikten sonra başarı döner. Bulunmayan bir anahtar için de silme kaydı yazılır. |
| `dbce_set_async` | Girdiyi kopyalar. Başarı, kuyruğun yazma isteğini kabul ettiğini gösterir. Kuyruk doluysa `false` döner ve `errno` değeri `EAGAIN` olur. |
| `dbce_sync_wait_checked` | Çağrı başladığında kuyruğa alınmış yazmaların tamamlanmasını bekler. Dosya açıldığından beri bu kapsamdaki ilk yazma hatasını bildirir. Sonraki bekleme çağrıları da aynı hatayı bildirir. |
| `dbce_get` | Değeri ve bayt uzunluğunu döndürür. İkili veriyi bu uzunlukla okuyun ve belleği `free()` ile serbest bırakın. Anahtar bulunamazsa `errno` değeri `ENOENT` olur. |
| `dbce_scan_all_checked` | Tarama başındaki kayıtları dolaşır. Geri çağırım aynı veritabanından okuyabilir veya ona yazabilir. Bazı kayıtlar işlendikten sonra okuma ya da bellek ayırma hatası oluşabilir. |
| `dbce_close_checked` | Kabul edilen yazmaları tamamlar, yazıcı iş parçacığının bitmesini bekler ve yazma veya kapatma hatalarını bildirir. Kapatmadan önce diğer çağrıların ve geri çağırımların bitmesini bekleyin. |

Her dosya için tüm süreçler genelinde tek bir açık veritabanı tanıtıcısı kullanın. İş parçacıkları bu tanıtıcıyı paylaşabilir. Kuyruk kilidi isteklerin kabulünü ve tamamlanmasını, ayrı bir kilit de dosyayı ve indeksi korur. Yazıcı tamamlanmayı bildirmeden önce dosya kilidini bırakır. Tarama geri çağırımları iki kilit de serbestken çalışır. İşaretçi veya mantıksal değer döndüren çağrılarda hata nedenini aynı iş parçacığında `errno` üzerinden okuyabilirsiniz.

### Dosya sınırları ve kurtarma

- Anahtarlar 1–63 baytlık C dizgeleridir. Değer en az bir bayt içermeli, uzunluğu `uint32_t` türüne sığmalı ve okuma sırasında sonlandırıcı NUL için ayrılan ek baytla birlikte `size_t` sınırını aşmamalıdır. Dosyada sıfır uzunluk silme anlamına gelir.
- İndeks, silinenler dahil 4096 farklı anahtar tutar. Sınırda mevcut anahtarlar güncellenebilir. Yeni bir anahtar eklemek `ENOSPC` hatası verir.
- Eski biçimin başlığında makinenin bayt sırasıyla yazılmış dört adet 32 bit alan bulunur: biçim imzası, ayrılmış CRC alanı, anahtar uzunluğu ve değer uzunluğu. Ardından sonlandırıcı NUL içermeyen anahtar ve ham değer gelir. Yeni kayıtlarda CRC sıfırdır. Uyumlu eski dosyalar dönüştürülmeden açılır.
- Açılışta kayıt sınırları denetlenir. Bozuk kayıt veya yarım kalmış son kayıt `EILSEQ` hatası verir. Dosya olduğu gibi korunur. Elle kurtarma için bir kopya alın veya sağlam bir yedeği geri yükleyin. Sona ekleme sırasında bir G/Ç hatası oluşursa o tanıtıcı üzerinden yeni yazmalar durur.
- Başarılı yazma, verinin işletim sistemine aktarıldığını gösterir. Güç kesintisinde onaylanmış veri kaybolabilir. Biçimde doğrulanan bir sağlama toplamı veya işlemin tamamlandığını gösteren bir işaret yoktur. Bu nedenle uzunluğu değişmeyen bir bozulma fark edilmeyebilir. Hata bildirilmiş olsa bile dosyaya tam yazılmış bir kayıt yeniden açılışta görünebilir.
