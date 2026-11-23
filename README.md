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
| `dbce_sync_durable` | Waits for checked completion, then calls `fsync` on Linux or `FlushFileBuffers` on Windows. Call once per batch or once per write. A failed durable sync is remembered until close. |
| `dbce_get` | Returns a caller-owned buffer and its byte length. Read binary data using that length and release the buffer with `free()`. A missing key sets `errno` to `ENOENT`. |
| `dbce_scan_all_checked` | Propagates earlier accepted write failures before visiting a snapshot. Callbacks can read or write through the same handle. A scan can report a read or allocation error after earlier callbacks have run. |
| `dbce_close_checked` | Drains accepted writes, joins the worker, and reports write or close errors. Finish other calls and callbacks before closing. |

Use one open handle per file across all processes. Threads can share that handle. The queue mutex protects admission and completion. A separate mutex protects the file and index. The worker releases the storage mutex before announcing completion, and scans invoke callbacks with both mutexes released. Failed calls returning a pointer or boolean set `errno` on the calling thread.

### Storage formats and maintenance

A new pathname creates V2. Existing V1 files, including zero-byte files, keep their original format on read and write. Both formats support 1–63 byte keys and 4096 distinct keys including tombstones. A value must fit a 32-bit length and leave room for a trailing NUL in the caller’s buffer. In V2, `dbce_set(db, key, NULL, 0)` stores an empty value. Get returns an allocated buffer of length zero, and scans include it. V1 reserves zero length for deletion.

V2 uses little-endian fields and verifies CRC-32/ISO-HDLC over the record metadata, key and value on open. A second CRC checks the header before recovery uses its lengths. V1 has structural validation only. See the [binary specification](docs/FILE_FORMAT.md).

Maintenance commands take literal paths, including the extension. Close the database first.

```sh
dbce migrate old.dbce new.dbce
dbce recover damaged.dbce
dbce recover damaged.dbce restored.dbce
```

Migration preserves the ordered history, checks the output against the source record by record, and publishes a new file. The first recovery command reports which part of the final V2 record is missing and how many bytes would be discarded. The second publishes the verified prefix as `restored.dbce` and the complete original bytes as `restored.dbce.bak`. Both names must be unused. Source files stay intact. A bad checksum, malformed metadata, or unsupported version fails recovery. A clean file has zero bytes to discard.

DBCE holds an OS lock until close. Another DBCE handle, including one in a different process, receives `EBUSY`. Use local filesystems that implement these locks and keep file paths under application control. Older binaries and external editors can bypass the locking protocol. CRCs are verified during opening and maintenance. Ordinary gets use the validated index.

Ordinary writes flush to the OS. Call `dbce_sync_durable` when a batch needs a storage barrier. Physical persistence depends on the filesystem, device and its write cache. An incomplete or complete unacknowledged last record is possible after interruption. See [platform publication limits](docs/FILE_FORMAT.md#publication-and-crash-boundaries) and [benchmark results](docs/BENCHMARK.md).

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
| `dbce_sync_durable` | Yazmaların sonucunu denetler, ardından Linux’ta `fsync`, Windows’ta `FlushFileBuffers` çağırır. Her yazmadan veya bir grup yazmadan sonra çağrılabilir. Eşitleme hatası kapatılana kadar saklanır. |
| `dbce_get` | Değeri ve bayt uzunluğunu döndürür. İkili veriyi bu uzunlukla okuyun ve belleği `free()` ile serbest bırakın. Anahtar bulunamazsa `errno` değeri `ENOENT` olur. |
| `dbce_scan_all_checked` | Önce kabul edilmiş yazmaların hatalarını bildirir. Başarılıysa tarama başındaki kayıtları dolaşır. Geri çağırım aynı veritabanından okuyabilir veya ona yazabilir. Bazı kayıtlar işlendikten sonra okuma ya da bellek ayırma hatası oluşabilir. |
| `dbce_close_checked` | Kabul edilen yazmaları tamamlar, yazıcı iş parçacığının bitmesini bekler ve yazma veya kapatma hatalarını bildirir. Kapatmadan önce diğer çağrıların ve geri çağırımların bitmesini bekleyin. |

Her dosya için tüm süreçler genelinde tek bir açık veritabanı tanıtıcısı kullanın. İş parçacıkları bu tanıtıcıyı paylaşabilir. Kuyruk kilidi isteklerin kabulünü ve tamamlanmasını, ayrı bir kilit de dosyayı ve indeksi korur. Yazıcı tamamlanmayı bildirmeden önce dosya kilidini bırakır. Tarama geri çağırımları iki kilit de serbestken çalışır. İşaretçi veya mantıksal değer döndüren çağrılarda hata nedenini aynı iş parçacığında `errno` üzerinden okuyabilirsiniz.

### Dosya biçimleri ve bakım

Yeni bir dosya yolunda V2 oluşturulur. Sıfır baytlık olanlar dahil mevcut V1 dosyaları açıldığında ve yazıldığında V1 biçimini korur. İki biçimde de anahtar uzunluğu 1–63 bayttır. İndeks silinenler dahil 4096 farklı anahtar tutar. Değer uzunluğu 32 bite sığmalı ve okuma tamponunda sonlandırıcı NUL için yer kalmalıdır. V2’de `dbce_set(db, key, NULL, 0)` boş bir değer kaydeder. Okuma, uzunluğu sıfır olan ayrılmış bir tampon döndürür ve tarama bu kaydı içerir. V1’de sıfır uzunluk silme anlamına gelir.

V2 alanları küçük sonlu bayt sırasıyla yazar. Açılışta kayıt bilgileri, anahtar ve değer üzerinde CRC-32/ISO-HDLC doğrulanır. İkinci bir CRC, kurtarmada uzunluklar kullanılmadan önce kayıt başlığını denetler. V1’de yalnızca dosya yapısı denetlenebilir. [İkili biçim tanımı](docs/FILE_FORMAT.md)

Bakım komutları uzantı dahil tam dosya adını alır. Önce veritabanını kapatın.

```sh
dbce migrate eski.dbce yeni.dbce
dbce recover hasarli.dbce
dbce recover hasarli.dbce kurtarilan.dbce
```

Göç işlemi geçmişi sırasıyla aktarır, her çıktı kaydını kaynakla karşılaştırır ve yeni dosyayı yayımlar. İlk kurtarma komutu, son V2 kaydının hangi kısmının eksik olduğunu ve kaç baytın atılacağını gösterir. İkinci komut doğrulanmış kayıtları `kurtarilan.dbce` dosyasına, kaynağın bütün baytlarını da `kurtarilan.dbce.bak` dosyasına yazar. İki dosya adı da boşta olmalıdır. Kaynak korunur. Sağlama toplamı hatası, geçersiz kayıt bilgisi veya desteklenmeyen sürüm kurtarmayı durdurur. Sağlam dosyada atılacak bayt sayısı sıfırdır.

DBCE, dosya kapanana kadar işletim sistemi kilidini tutar. Aynı dosyayı ikinci kez açmaya çalışan DBCE tanıtıcısı, başka bir süreçte olsa da `EBUSY` alır. Bu kilitleri destekleyen yerel bir dosya sistemi kullanın. Eski sürümler ve harici düzenleyiciler kilit protokolünü atlayabilir. CRC denetimi açılışta ve bakımda yapılır. Normal okumalar doğrulanmış indeksi kullanır.

Normal yazmalar veriyi işletim sistemine aktarır. Bir grup yazmayı depolama aygıtıyla eşitlemek için `dbce_sync_durable` çağırın. Güç kesintisindeki sonuç dosya sistemine, aygıta ve yazma önbelleğine bağlıdır. Kesintiden sonra son kayıt yarım kalabilir veya onay alınmadığı halde tam olarak bulunabilir. [Platform sınırları](docs/FILE_FORMAT.md#publication-and-crash-boundaries) · [Performans ölçümleri](docs/BENCHMARK.md)
