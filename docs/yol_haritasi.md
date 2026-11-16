# Navigator — File Pilot ilhamlı Linux dosya yöneticisi yol haritası

**Çalışma adı:** Navigator  
**Belge tarihi:** 16 Kasım 2026 (Europe/Istanbul)  
**Kaynak kesiti:** File Pilot genel beta **v0.8.5** (31 Ağustos 2026, 2,46 MB, Windows 7–11, x86-64)  
**Uygulayıcı düzen:** Koordinatör model **Fable 5.2** işi planlar ve **Claude Code alt ajanlarına** dağıtır. Bu belge Fable 5.2 veya kapasiteleri hakkında varsayımda bulunmaz; onu yalnızca orkestratör olarak ele alır.  
**Dil:** Anlatım Türkçe. Tüm koordinatör / alt ajan promptları ve `CLAUDE.md` taslağı İngilizce (yapıştırıma hazır).

---

## 0. Bağımsız proje uyarısı

Bu yol haritası, [File Pilot](https://filepilot.tech/) ürününün **resmi bir Linux portu değildir**. File Pilot, Voidstar / Vjekoslav Krajačić tarafından geliştirilen, kapalı kaynaklı ticari bir Windows uygulamasıdır. Resmi sitedeki indirme SSS’sine göre Linux ve macOS portları *onların* yol haritasındadır ve ancak Windows deneyimi sağlamlaştıktan sonra ele alınacaktır. Kaynak kodu açık kaynak yapılmayacaktır; ileride “Source Included” ayrı bir satış olarak planlanmaktadır.

Navigator:

- File Pilot’ın **doğrulanmış UX ve özelliklerini** ve resmi yol haritasındaki henüz çıkmamış maddeleri referans alan **bağımsız** bir Linux dosya yöneticisidir.
- File Pilot’ın adı, logosu, görselleri, ikonları veya ikili dosyası kopyalanmamalıdır. İlk herkese açık yayından önce benzersiz bir ürün adı seçilmelidir (“Navigator” geçici çalışma adıdır).
- Tersine mühendislik veya tescilli koda bakılarak geliştirme yapılmaz. Referans: resmi site, Starlog, yol haritası, kamuya açık incelemeler ve teknik konuşmalar.

---

## 1. File Pilot nedir? (kısa özet)

File Pilot, 2022’den beri tam zamanlı geliştirilen, “sıfırdan, düşük seviyeli, yerel” bir Windows dosya gezginidir. Konumu: **çok hızlı, küçük, tek yürütülebilir dosya, klavye ve fare eşitliği, özelleştirilebilir arayüz.**

Resmi bildirideki beş ilke ([about](https://filepilot.tech/about)): **Fast** (gecikmesiz arayüz, anında açılış), **Modern** (paneller, sekmeler, inspector, komut paleti), **Personal** (renk, kısayol, yerleşim, yazı tipi, boşluk, animasyon), **Standalone** (tek küçük EXE; arka plan hizmeti yok; kurucu isteğe bağlı ve aynı dosyanın içinde), **Yours** (süreli abonelik ve hesap yok; çevrimdışı kalıcı lisans).

Teknik konuşmalara ve geliştirici yayınlarına göre (Handmade / Wookash, BSC 2025 “Inside the Engine”, Hanselminutes #1030) uygulama büyük ölçüde **C (C11)** ile yazılmıştır. Mimari üç katmandır: Windows platform kodu, çizim katmanı (önce OpenGL, 0.7.1’den itibaren varsayılan **Direct3D**), ve kodun yaklaşık **%80’ini** oluşturan platformdan bağımsız program katmanı. Arayüz **immediate-mode**’dur; her karede yeniden çizilir. Girdi komutları kuyruğa alınır ve **kare sınırında** işlenir. Bellek **arena**’larla yönetilir. Derleme süresi geliştirici tarafından “yaklaşık 3 saniye” olarak anlatılmıştır.

v0.8.5 itibarıyla genel beta ücretsizdir. Sitenin verdiği ölçek: 400 bin+ indirme, binlerce lisans sahibi, 10 bin+ topluluk üyesi. Lisans modeli kalıcıdır (Essential/Team: v1’den sonra bir yıl güncelleme; Pro/Team Pro: geliştirme sürdükçe güncelleme, VIP / Insider). Fiyatlar bu belgenin konusu değildir; değişebilir, resmi [pricing](https://filepilot.tech/pricing) sayfasına bakılmalıdır.

**Halindeki çekirdek vaat (Navigator’in de hedefi):** klasör açmak, aramak, kaydırmak, önizlemek ve bağlam menüsünü açmak “düşünceden hızlı” hissi vermeli; arayüz tek karede yanıt vermeli; uygulama küçük, yerelde çalışan, hizmetsiz bir ikili olmalıdır.

---

## 2. Kaynaklar

Aşağıdaki liste 16 Kasım 2026’da açılmış sayfalara dayanır. **Uydurma özellik yoktur.** Resmi kılavuz sayfası ([manual](https://filepilot.tech/manual)) “yapım aşamasında”dır ve içerik sunmaz.

### Resmi

| Sayfa | Not |
|---|---|
| https://filepilot.tech/ | Ana özellikler, v0.8.5, 2,46 MB |
| https://filepilot.tech/about | Manifesto, ekip, teknik konuşmalara giriş |
| https://filepilot.tech/starlog | Changelog (2025–2026); 2025 arşivi: `/starlog/2025` |
| https://filepilot.tech/roadmap | 1.0 yolu, Future, Launched |
| https://filepilot.tech/download | İndirme, platform SSS (Unicode, ağ, ARM, VM/Wine, Linux/macOS) |
| https://filepilot.tech/pricing | Lisans / fiyat SSS |
| https://filepilot.tech/manual | Boş (“in progress”) |

Önemli Starlog girişleri: [v0.5.0](https://filepilot.tech/starlog/2025-07-07-v0.5.0), [v0.6.0](https://filepilot.tech/starlog/2025-11-08-v0.6.0), [v0.8.1](https://filepilot.tech/starlog/2026-07-15-v0.8.1) (Tree / Columns / Group by / hiyerarşik görünümler).

### İnceleme ve tartışma (bağımsız; resmi iddia değildir)

- MakeUseOf, Tashreef Shareef, 11 Eylül 2026 — https://www.makeuseof.com/file-pilot-windows-file-manager-free/
- Andrew Lock, 13 Ocak 2026 — https://andrewlock.net/windows-explorer-replacement-filepilot-is-awesome/
- dtptips / Meera Joshi, 13 Ocak 2026 — https://dtptips.com/exploring-file-pilot-a-modern-powerful-alternative-to-windows-file-explorer/
- iTechGuides, 18 Ağustos 2026 (v0.8.3 kesiti) — https://www.itechguides.com/hands-on-file-pilot-may-be-the-best-file-explorer-alternative-yet/
- XDA, João Carrasqueira, 30 Temmuz 2026 (arama karşılaştırması) — https://www.xda-developers.com/used-everything-windows-search-file-pilot-find-lost-files-fast-theres-one-clear-winner/
- Hacker News: https://news.ycombinator.com/item?id=43091466
- Reddit: r/Windows11 tartışmaları (ör. “Tried File Pilot Today…”, “File Pilot is simply Incredible!”)
- Kısayol derlemesi (eksik, üçüncü taraf): https://defkey.com/file-pilot-0-7-shortcuts — sayfa tam liste vermedi; **kesin kısayol listesi olarak kullanılmasın**

### Teknik konuşmalar

- Hanselminutes #1030, 1 Ocak 2026 — “Vjekoslav Krajačić on File Pilot and a return to fast UIs”
- BSC 2025 — “File Pilot: Inside the Engine” (arena / C pratikleri)
- Wookash Podcast — “Why File Pilot Compiles in 3 Seconds” (katmanlar, komut kuyruğu, arena, C11)

İnceleme tarihleri önemlidir: Ocak 2026 yazıları “Group by yok” diyebilir; bu özellik resmi olarak **Temmuz 2026 / v0.8.1** ile gelmiştir. MakeUseOf’un “LAN’e erişemez / varsayılan gezgin yapılamaz” notları, resmi SSS ve kurucu seçenekleriyle çelişebilir (ağ: eşlenmiş sürücü + UNC var, kenarlar var; varsayılan gezgin: kurucuda isteğe bağlı).

---

## 3. Doğrulanmış özellik listesi

Her madde en az bir kaynağa bağlıdır. `R` = resmi site / Starlog / roadmap / download SSS. `İ` = inceleme veya topluluk (davranış doğrulanmış sayılır ama resmi spesifikasyon değildir).

### 3.1 Performans ve dağıtım

| Özellik | Kaynak |
|---|---|
| Çok düşük arayüz gecikmesi; “klasörler tıklanmadan açılıyor” hissi (pazarlama + yaygın ilk izlenim) | R, İ |
| Tek, küçük bağımsız yürütülebilir (~2,46 MB v0.8.5); isteğe bağlı kurucu aynı dosyada; arka plan hizmeti yok | R |
| Windows 7–11, x86-64; resmi ARM yok (emülasyon garanti değil) | R FAQ |
| VM’de Direct3D ile çalışır, grafik performansı düşer; Wine resmi olarak test/destek edilmez | R FAQ |
| İsteğe bağlı sistem entegrasyonu: Win+E, klasörü çift tıklayınca FP, sağ tık “Open in File Pilot” / “File Pilot here”, PATH | R FAQ, İ (Andrew Lock) |
| Kullanıcı verisi JSON; örnekler arası hot-reload; bilinmeyen alanlar atlanır | R Starlog |
| SSE4.1 denetimi (eski CPU) | R v0.5.0 |

### 3.2 Paneller, sekmeler, yerleşimler

| Özellik | Kaynak |
|---|---|
| İstenen panel yerleşimi; dikey ve yatay bölme; her panelin kendi sekme kümesi | R, İ (MUO, Andrew Lock) |
| Sekmeleri sürükleyerek sola/sağa/üste/alta bölme; ayırıcıları sürükleyerek boyut | R, İ (dtptips) |
| Çoklu panel (yalnızca iki panel değil; ultrawide’da ekran yettiğince) | İ MUO |
| Sekmeler: yeni, kapat, orta tık ile kapat, orta tık “yeni sekme” düğmesi | R, İ |
| `Ctrl+T` yeni sekme, `Ctrl+W` kapat (İ); yeni sekme varsayılan olarak *geçerli* sekmenin klasörünü açar (R v0.5) | İ / R |
| Sekme çoğaltma; `Ctrl+N` “Duplicate Tab in New Window” (sekme kaynak pencerede kapanmaz) | R |
| `Ctrl` + tık / `Ctrl` + çift tık ile yeni sekmede açma (Quick Access ve panel) | R v0.6.0 |
| Oturumda sekmeleri ve yerleşimi hatırlama; kaydedilip yüklenen layout’lar | R, İ |
| Search Tabs açılır listesi: açık ve yakınlarda kapatılmış sekmeler, hızlı yeniden açma | R Roadmap 0.5, Starlog |
| Close Left Tabs; sekme alanına sağ tık “Tabs Options” | R v0.5.0 |
| Pencere konumunu yeniden açılışta hatırlama | R v0.6.0 |
| Aktif olmayan panellerin hafifçe sönmesi | İ dtptips |

### 3.3 Arama ve filtre

| Özellik | Kaynak |
|---|---|
| Geçerli klasörde anlık filtre (File Pilot bunu “filtering” olarak ayırır) | R, İ XDA |
| `Ctrl+E` ile hiyerarşiyi düzleştirme (alt klasörler / tüm sürücü) | İ XDA; R “flattened folder hierarchies, including entire drives” |
| Bulanık (fuzzy) dosya adı araması | R |
| Uzantıya / türe göre filtre (types popup, tümünü aç/kapa, gizli/sisteme saygı) | R, İ |
| Milisaniyeler mertebesinde sonuç iddiası (resmi pazarlama); XDA’da tüm sürücü için “genelde 9 saniyenin altı” — bu bir laboratuvar ölçümü değildir | R / İ |
| Joker (`*`) yok (XDA, resmi listede de yok) | İ, doğrulanmış yokluk |
| İçerik (dosya içi) araması resmi özellik olarak geçmez | doğrulanmış değil; yok varsayılır |

### 3.4 Inspector (önizleme)

| Özellik | Kaynak |
|---|---|
| `Space` ile açılır (yaygın İ); `Alt` + numpad `Enter` de Inspect (R) | İ / R |
| Metin, görüntü ve klasör içeriğine bakma (klasör neredeyse canlı alt görünüm) | R, İ |
| Görüntü: EXIF yönü, pencere boyutuna göre çözünürlük, sığdır / 1:1, ızgara, mini-harita | R 0.7.x, İ dtptips |
| Zengin metadata panelleri; kilitlenebilir inspector; konum pencereye göre sağ veya alt, zorlanabilir | İ dtptips |
| PDF ve SVG önizlemesi yok (birden fazla inceleme, v0.8 dönemine kadar) | İ |
| Video: `.mp4` boyut bilgisi Starlog’da geçer; tam video oynatma önizlemesi resmi özellik olarak doğrulanmadı | kısmen; oynatma iddia edilmesin |

### 3.5 Toplu yeniden adlandırma

| Özellik | Kaynak |
|---|---|
| Birden fazla dosyayı etkileşimli (önizlemeli) yeniden adlandırma | R |
| Benzersiz kimlik üretme, dosya tarihini kullanma | R |
| Toplu ada tek satır yapıştırma özel durumu | R v0.5.0 |
| Yeniden adlandırırken baştaki/sondaki boşlukların kırpılması | R v0.8.5 |

### 3.6 GoTo ve adres çubuğu

| Özellik | Kaynak |
|---|---|
| GoTo: sık sistem yolları, son ziyaret edilenler, otomatik tamamlama ile gezinme | R |
| Ortam değişkenleri (`%VAR%`); sondaki `\` olmasa da genişletme (v0.8.5 düzeltmesi) | R |
| Yol büyük/küçük harf otomatik düzeltmesi | R v0.6.0 |
| Breadcrumb; ikonlar; açılır listede önizleme ve öge sayısı (İ dtptips) | İ / R (breadcrumb anahtarı v0.6.0) |
| Compact Paths / Path Breadcrumbs, adres çubuğu sağ tık menüsünde | R |
| Kilit simgesi: klasör / hiyerarşi / sistem geneli görünüm ayarı | R 0.8.1, İ |
| Geri / ileri / yukarı; yer imi düğmesi; boş başlık alanına hover ile GoTo | R, İ |
| Adres çubuğundan yol yapıştırma / yazma; `Ctrl+Enter` ile açma (İ) | İ |

### 3.7 Komut paleti ve kısayollar

| Özellik | Kaynak |
|---|---|
| Genel komut paleti; eylemleri arama; kısayol atama | R |
| Numpad, tuş dizileri (key sequences / “chain hotkeys”), takma adlar (aliases) | R |
| Varsayılan açılış: `Ctrl+Shift+P` (İ, VS Code ile aynı) | İ |
| Ayarlar: `Ctrl+,` (İ) | İ |
| Neredeyse tüm eylemler kısayollanabilir; UI kısayolları gösterir | İ Andrew Lock |
| F13–F24 kabul edilir | R v0.6.0 |
| Zoom seviyelerine ayrı kısayol | R v0.5.0 |
| `Ctrl+Space` imleç seçimini aç/kapa (Explorer varsayılanı; eskiden `Shift+Enter`) | R |
| Numpad Enter = Activate | R |
| Kenar çubuğu gizle/göster: `Ctrl+B` (İ dtptips) | İ |
| Klasör filtresi odaktayken çoğu komut yine tetiklenebilir | R |

Tam varsayılan keymap resmi kılavuzda yayınlanmamıştır. Yukarıdakiler dışında kısayol uydurulmamalıdır.

### 3.8 Bağlam menüsü

| Özellik | Kaynak |
|---|---|
| Sistem ve üçüncü taraf eylemleri; menü içinde arama; sık kullanılanları sabitleme (pin) | R, İ MUO |
| Sağ sürükleme menüsünde üçüncü taraf uzantı öğeleri (v0.7.0) | R |
| Klasör arka planına / öğe içi boş alana sağ tık ile klasör menüsü | R 0.8.1 |
| “Pin to Quick Access” ve “Give Access To” FP menüsünden çıkarılmış (v0.5) | R |

### 3.9 Görünümler ve liste

| Özellik | Kaynak |
|---|---|
| Ayrıntılar (Details): sütunlar yeniden sıralanır, Name de dahil (0.8.1); boş meta sütunları | R |
| Simge / ızgara; `%` kaydırıcısı veya `Ctrl`+tekerlek ile anında ölçek | İ MUO |
| List / Columns view (0.8.1, Explorer List benzeri dikey sütunlar) | R |
| Tree view (0.8.1) | R |
| Group by: tarih, tür, boyut; daraltılabilir gruplar (0.8.1) | R |
| Hiyerarşik klasör görünümleri: klasör / ağaç / varsayılan (0.8.1) | R |
| Klasörler önce veya dosyalar önce; karışık sıralama seçeneği | R, İ |
| Sıralama: ad, boyut, tarih, sayı, birim/klasör kullanımı; tarih ve boyutta varsayılan azalan | R |
| Tür görünürlük süzgeçleri oturumda ve kayıtlı yerleşimde saklanır | R |
| Seçili dosyaların boyutunu toplama; birim ve klasör kullanım yüzdesi + çubuk | R |
| Boş / gizli klasör içeriği için etiket | R |
| Alternating row colors (0.8.1’den varsayılan açık) | R |
| Scroll beyond last item (0.8.1’den varsayılan kapalı) | R |
| Dikdörtgen seçim; onay kutularında sürükleyerek çoklu seçim (0.8.1) | R |
| Klasöre ilk girişte seçim yok; kaydırma konumu korunur | R |
| “Places” altında sürücü doluluk çubuğu | R |

### 3.10 Kenar çubuğu, yer imleri, son kullanılanlar

| Özellik | Kaynak |
|---|---|
| Bölümler: Recents, Bookmarks, Places, Storage; Quick Access | İ dtptips, R |
| Birim kullanım çubuğu / yüzde | R (Temmuz 2025), İ |
| Bölümleri daraltma; yer imleri; çevrimdışı yer imleri ve recents kalır, birim dönünce senkron | R v0.6 |
| Recents’i üste sıralamadan vurgulama; “Sort Recents On Top” anahtarı; recents izlemeyi kapatma | R |
| Kısayol/link overlay ikonları; uzantı ayarına uyma; silme/ad değişiminde güncelleme | R v0.5.0 |
| Quick Access gruplarını serbest sürükleyerek düzenleme resmi 1.0 sonrası “Future” maddesidir (şimdi yok) | R Roadmap Future |

### 3.11 Dosya işlemleri ve sürükle-bırak

| Özellik | Kaynak |
|---|---|
| Kopyala, kes, yapıştır, sil, yeniden adlandır, yeni klasör (`Ctrl+Shift+N` İ) | İ / beklenen temel |
| Copy as Path; seçim yoksa geçerli klasör yolu; bağlam duyarlı tırnak | R |
| Önceki panele kopyala/taşı kısayolu | R v0.5.0 |
| Sürükle-bırak: dış uygulamalardan (tarayıcı, e-posta), ZIP arşivinden dışarı, yürütülebilir/betik üzerine bırakma (v0.7.0) | R |
| Sürüklerken tekerlekle kaydırma; kenarda otomatik kaydırma | R |
| Modern Windows aktarım iletişim kutusu (0.7.1) | R (Windows’a özgü) |
| Open Console; kısayol/link’in hedef klasörünü açma | R |
| Undo / Redo **yok** (resmi 1.0 planı; Reddit’te eksiklik olarak geçiyor) | R Roadmap |

### 3.12 İzleme, dizin, ağ, birimler

| Özellik | Kaynak |
|---|---|
| Dizin izleme; v0.6’dan itibaren sürücü genelinde `ReadDirectoryChanges` | R |
| Birim çıkarma / USB kopması / yeniden takılma | R |
| Desteklenen sürücülerde `NtQueryDirectoryFile` ile listeleme hızı | R v0.6.0 |
| Eşlenmiş ağ sürücüleri ve UNC (`\\sunucu\paylaşım`); bazı SMB / TrueNAS / kimlik sorunu kenarları | R FAQ |
| Ağ paylaşımlarında klasör boyutu hesabı ayarı | R v0.6.5 |
| “Advanced file indexing” (NTFS MFT) **Future**, henüz yok | R Roadmap |

### 3.13 Küçük resimler, ikonlar, önbellek

| Özellik | Kaynak |
|---|---|
| İkon / thumbnail / preview önbelleği; Options’ta Asset Cache Size | R 0.7.x |
| Thumbnail yükleme zamanı seçenekleri; sığdır / doldur; yenileme önbelleği zorla yeniler | R |
| Overlay ikonlar (Tortoise SVN vb. için istek karşılanmış) | R v0.5.0 |
| Klasör görsellerini inspector için prefetch | R v0.6.0 |
| Animasyon kapalıyken thumbnail fade de kapalı | R v0.8.2 |

### 3.14 Özelleştirme, tema, ayarlar

| Özellik | Kaynak |
|---|---|
| Yazı tipi boyutu ve aralık; animasyonu tek tıkla kapatma; renk şemaları | R |
| Özel renk şeması JSON | R v0.8.3 |
| Açık / koyu; MUO’ya göre koyu tema elle açılır (otomatik de İ dtptips’te geçiyor — çelişki, varsayılan doğrulanmamış) | İ |
| Panel saydamlığı (İ MUO) | İ |
| Yuvarlatılmış köşeler; dikdörtgen pencere düğmeleri seçeneği | İ, R |
| İpucu (tooltip) aç/kapa, gecikme | R |
| Gizli / sistem / nokta dosyaları; uzantıları göster; long path | R |
| Tek tıkla aç (yalnızca klasör seçeneği), altı çizgiyi kapatma | R v0.5.0 |
| Boş arka plana çift tık ile üst klasöre çık | R |
| Başlangıç konumu: ev, kök, son oturum; yeni sekme davranışı | İ dtptips |
| Ayar paneli aranabilir (`Ctrl+,`) | İ |

### 3.15 Dil, Unicode, diğer sınırlar (resmi)

| Madde | Durum | Kaynak |
|---|---|---|
| Tam Unicode | Yok; Latin ve Kiril çalışır; Çince / Korece / Japonca düzgün görünmez | R FAQ |
| Yerelleştirme (UI dili) | 1.0 planı, henüz yok (konuşmalarda şimdilik İngilizce) | R Roadmap |
| Eklenti / üçüncü taraf uzantı API’si | Yok (İ MUO) | İ |
| Komut satırı argümanları (`--new-tab` vb.) | 1.0 planı; PATH’e ekleme kurucuda var | R |
| Miller columns, çok satırlı ad, daha büyük thumbnail | 1.0 planı | R |
| Sanal klasörler, MTP, bulut durumu, otomatik güncelleyici, çevrimiçi kılavuz | 1.0 planı | R |

---

## 4. Uydurulmayan / doğrulanmamış noktalar

Şunları “File Pilot özelliği” diye yazmayın; Navigator’de istiyorsak “daha fazlası” olarak işaretleyin:

- Dosya *içi* (içerik) arama, regex veya Everything tarzı joker dili
- PDF / SVG / evrensel video oynatıcı inspector
- Yerleşik geri al / yinele (resmi planda, ürünün bugünkü halinde yok)
- Eklenti pazarı
- Tam varsayılan keymap (yayımlanmış resmi liste yok)
- PowerToys Peek / QuickLook entegrasyonu
- Sistem dosya aç/kaydet diyaloglarının değiştirilmesi (Windows’ta da mümkün değil)
- Başka uygulamadan klasör açılınca *mevcut* pencereye sekme olarak ekleme (Andrew Lock bunu eksiklik olarak yazdı)
- İzleme dört yönlü jestleri (İ: yok)
- Resmi performans kıyas tablosu (site iddiası ölçüm değildir)

---

## 5. “Daha fazlası” — Linux’a özgü ve File Pilot’ı aşan hedefler

Bunlar Navigator’in bilinçli farkıdır. Bir kısmı File Pilot’ın kendi 1.0 / Future listesindeki boşlukların Linux karşılığıdır.

1. **Wayland + X11.** Birincil: GNOME / Linux Mint Wayland. X11 ve XWayland yedek. Kesirli ölçek, HiDPI, metin IME (Türkçe, CJK).
2. **Tam Unicode + HarfBuzz.** File Pilot’ın CJK açığını kapatmak; NFC/NFD, uzun UTF-8 yollar.
3. **GVfs / gvfsd.** MTP (telefon, fotoğraf makinesi, USB müzik çalar), SMB, SFTP, gvfs mount’ları, FUSE, `/run/user/.../gvfs`.
4. **XDG Trash** (FreeDesktop trash spec) ve **Undo/Redo** yığını (File Pilot’da henüz yok).
5. **xdg-open ve MIME.** `inode/directory` varsayılan gezgin; `.desktop`; portal; `xdg-mime`.
6. **FreeDesktop Thumbnail Spec** (`~/.cache/thumbnails`), paylaşılan önbelleğe yazma/okuma.
7. **inotify** (gerekirse `fanotify`) ile anlık dizin güncellemesi; birim bağlama/çıkarma (`udisks2` / udev).
8. **Git durumu** overlay (modified / untracked / ignored) ve isteğe bağlı kısa dal adı.
9. **Uçbirim.** “Bu klasörde uçbirim aç” (kullanıcının varsayılanı: gnome-terminal, kitty, ptyxis, x-terminal-emulator) ve isteğe bağlı alt panel (ilk sürümde zorunlu gömülü VTE yok).
10. **Arşivler.** ZIP’ten sürükleme (FP v0.7) + tar.gz / tar.xz / 7z okuma, oluşturma, içine göz atma.
11. **Kalıcı dizin + içerik araması.** Everything benzeri anlık ad indeksi (walk + inotify) ve dosya içi arama (ripgrep benzeri).
12. **Tek örnek + IPC.** Dışarıdan açılan yol, çalışan pencereye yeni sekme olarak düşer (FP’de eksik olduğu belirtilmişti).
13. **Eklenti API’si** (FP’de yok): komut, önizleme, overlay, sütun; kararlı C ABI + isteğe bağlı JSON manifest.
14. **PDF / SVG / medya önizleme** (FP’de eksik): PDF (poppler veya pdfium), SVG, isteğe bağlı kısa video karesi (ffmpeg).
15. **Miller columns** (FP 1.0 planı).
16. **CLI:** `navigator`, `navigator PATH`, `navigator --new-tab PATH`, `navigator --select PATH`.
17. **i18n:** ilk günden Türkçe ve İngilizce arayüz.
18. **Paketlerme:** `.deb` (Mint / Ubuntu), AppImage, Flatpak; isteğe bağlı güncelleme denetimi (zorunlu telemetri yok).
19. **Sağlama (checksum)** ve izin / sahip / xattr paneli.
20. **Erişilebilirlik (AT-SPI)** — özel GPU UI olduğu için fazlı, sonlara bırakılır; komut paleti ve tam klavye ile iş yapılabilir olmalıdır.

---

## 6. Önerilen teknoloji yığını

**Seçim: modern C++20/23, CMake, SDL3 (pencere / girdi / DnD / yayın), SDL_GPU veya OpenGL 3.3 yedekli özel immediate-mode arayüz, metin için FreeType + HarfBuzz, bağımlılıklar için vcpkg (dağıtım `.deb`inde mümkünse sistem kütüphaneleri), test için Catch2, clang-format + clang-tidy.**

Dear ImGui, **başlangıç iskeleti ve geliştirici araçları** için kullanılır; File Pilot hissi veren asıl dosya listesi, sekmeler ve inspector **özel, sanal (virtualized) widget’lar** olarak yazılır. Varsayılan ImGui “debug aracı” görünümü ürün UI’si değildir.

### Neden bu yığın?

File Pilot’ın imzası “çok hızlı, çok küçük, yerelde çizilen arayüz”dür: özel GPU, immediate-mode, kare sonunda komut kuyruğu, tek küçük ikili. Geliştiricinin kendi anlattığı katmanlar (platform / renderer / program) Navigator’e doğrudan uyar.

- **Qt6 / QML:** Wayland ve araç kutusu olgun, ama ikili ve çalışma zamanı büyük (onlarca MB), tema ve milyonluk sanal listede File Pilot yoğunluğunu vermek Qt ile kavga demektir. “Tiny standalone” vaadi bozulur.
- **gtkmm4:** Mint/GNOME’da yerli durur, GVfs kolaydır; fakat özel tema, sekme-sürükleyip-bölme, 60 fps thumbnail zoom ve “animasyonu tek tuşla kapat” davranışında GTK widget modeli ayak bağı olur. Nemo/Nautilus zaten bu yolda.
- **C++ + SDL3 + özel IMUI:** File Pilot’ın C + OpenGL/D3D yaklaşımına en yakın seçimdir. SDL3’ün Wayland ve X11 desteği birinci sınıftır; pano, sürükle-bırak, HiDPI vardır. SDL_GPU ile Vulkan (Wayland) ve OpenGL yedek kurulur. C++20/23, File Pilot’ın arena/kuyruk modelini `std::span`, kendi arena/havuz tipleri ve `std::jthread` ile kurmaya yeter. Claude Code bu yığında üretkendir.

**Dürüst boyut hedefi:** 2,46 MB’ye (C, sistem fontu yok, eksik Unicode) yetişmek gerçekçi değildir. Hedef: şeritli, LTO’lu, sistem fontunu kullanan sürümde **yaklaşık 8–25 MB**; AppImage/Flatpak paketleri runtime ile daha büyük olabilir. Açılış “hemen”, ilk pencere bir karede.

**Neden düz C değil?** File Pilot Starlog’u access violation’larla dolu. Uzun ömürlü bir gezginde C++ (RAII, `enum class`, tek bir `navigator::Result` / `std::expected` politikası) aynı mimariyi daha az patlatır.

### Ana kütüphaneler

| Parça | Seçim | Not |
|---|---|---|
| Dil | C++20 taban, C++23 nerede derleyici izin verirse | MSRV: GCC 13 / Clang 17 / libstdc++ |
| Yapı | CMake 3.25+, Ninja, preset’ler | `clang-tidy` ve `clang-format` CI’da |
| Pencere / girdi | SDL3 | Wayland + X11; tek çeviri birimi sarmalayıcı |
| Çizim | SDL_GPU (Vulkan) + OpenGL 3.3 yedek | Immediate-mode liste; atlas |
| UI çekirdeği | İnce özel IMUI + Dear ImGui (iskelet) | Komut kuyruğu kare sonunda (FP modeli) |
| Metin | FreeType, HarfBuzz, isteğe bağlı SheenBidi | Tam Unicode |
| Görüntü | stb_image / libpng / libjpeg-turbo; SVG ayrı | EXIF: libexif veya benzeri |
| PDF | poppler-cpp veya pdfium, isteğe bağlı derleme | Faz 6 sonrası |
| İzleme | inotify, udev/udisks2 | |
| GVfs | GIO (glib) ince sarmalayıcı, çekirdekten yalıtılmış | `platform` modülü |
| Arama | Kendi ad indeksi + fuzzy; içerik ayrı | |
| Arşiv | libarchive | zip/tar/7z okuma |
| Test | Catch2 | Birim + geçici dizin altın testleri |
| Bağımlılık | vcpkg manifest (geliştirme); `.deb` için sistem `.so` tercih | |
| Paket | CPack DEB, linuxdeploy AppImage, Flatpak manifest | |

Sistem kütüphaneleri (`libsdl3`, `libfreetype`, `libharfbuzz`, `libgio`) `.deb` için tercih edilir; AppImage/Flatpak demetler.

---

## 7. Depo yapısı, modül sahipliği, sözleşmeler

Fable 5.2 ve alt ajanlar **önce başlık, sonra gövde** kuralına uyar. Bir modülün `include/navigator/*.hpp` dosyası, o modülün API sözleşmesidir. Başka bir ajan yalnızca bu başlıklara (ve `docs/architecture.md`’ye) dayanır; komşu `src/**` dosyalarına “hızlıca bakıp” alan ihlali yapmaz.

### 7.1 Dizinler

```
navigator/
  CLAUDE.md
  docs/architecture.md
  docs/features.md
  include/navigator/          # kamu sözleşmeleri (headers-first)
  src/app/                # main, döngü, bileşim kökü
  src/core/               # fs, volume, watch, path
  src/ops/                # copy/move/trash/undo
  src/search/             # filter, flatten, fuzzy, index
  src/preview/            # inspector, thumbnail, dekoder
  src/commands/           # registry, palette, keymap
  src/ui/                 # IMUI, paneller, sekmeler, tema
  src/platform/           # Linux: xdg, gio, udev, ipc
  src/plugins/            # C ABI, loader
  tests/
  assets/
  packaging/              # deb, appimage, flatpak
  .clang-format
  .clang-tidy
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  .github/workflows/ci.yml
  .claude/agents/*.md
```

### 7.2 Modül sahipliği

| Alt ajan | Sahip olduğu yollar | Dokunmaz |
|---|---|---|
| `contracts` (kısa ömürlü, Faz 0 ve API evrimi) | `include/navigator/**`, `docs/architecture.md` | `src/**` uygulamaları (iskelet `.cpp` hariç) |
| `core-fs` | `src/core/**`, `tests/core/**` | UI, preview çizimi |
| `platform` | `src/platform/**`, `tests/platform/**` | UI widget’ları |
| `ui` | `src/ui/**`, `src/app/**` (app loop eşgüdümle), `assets/**` | indeks algoritması, GIO ayrıntısı |
| `search` | `src/search/**`, `tests/search/**` | widget çizimi |
| `preview` | `src/preview/**`, `tests/preview/**` | fs yazma işlemleri |
| `ops` | `src/ops/**`, `tests/ops/**` | tema / layout |
| `commands` | `src/commands/**`, `tests/commands/**` | dekoderler |
| `packaging` | `packaging/**`, CPack, CI yayın işleri, `.desktop` | özellik kodu (izin olmadan) |
| `qa` | `tests/**` genişletme, hata ayıklama listesi, fuzz | özellik sapması (önce issue) |
| `design-reviewer` | inceleme notu (dosya yazmaz veya yalnızca `docs/review/**`) | kod birleştirmez |

Çakışan dosyalar (`src/app/main.cpp`, kök `CMakeLists.txt`) **koordinatöre aittir** veya tek bir ajan “integration” geçişinde dokunur. Paralel ajanlar kendi `src/<modül>/CMakeLists.txt` dosyalarını düzenler; kök CMake’e eklenecek satırı `docs/integration-notes.md` içine yazar, birleştirme ajanı uygular.

### 7.3 Sözleşme ilkeleri (headers first)

1. Yeni özellik: önce `include/navigator/<modül>.hpp` ve varsa küçük değer tipleri. Fable 5.2 bu başlığı onaylamadan paralel uygulama başlamaz (veya aynı anda yalnızca o başlığı yazan `contracts` ajanı çalışır).
2. Başlıklar: sızıntısız (`SDL.h` kamu başlığına sızmaz), ileri bildirim tercih, thread-safety notu yorumda, hata tipi `navigator::Result<T>` / `std::expected` tek politika.
3. UI, dosya sistemini doğrudan `std::filesystem` ile dolaşmaz; `navigator::fs` ve `navigator::Index` kullanır.
4. Komutlar `navigator::CommandId` ve `navigator::CommandQueue` üzerinden yürür. Widget kodu kare içinde yan etki yapmaz; komut kuyruğa eklenir (File Pilot modeli).
5. API değişince changelog notu; kırıcı değişiklik koordinatör onayı ister.

Örnek kamu başlıkları (Faz 0’da iskelet olarak yazılır, imzalar evrilir):

- `include/navigator/types.hpp` — `Path`, `FileId`, `Result`, `Span`, `Arena`
- `include/navigator/fs.hpp` — listeleme, metadata, sıralama, symlink, yetki
- `include/navigator/watch.hpp` — dizin / birim olayları
- `include/navigator/volume.hpp` — bağlama noktaları, kullanım
- `include/navigator/index.hpp` / `search.hpp`
- `include/navigator/ops.hpp` — kopya/taşı/çöp/geri al
- `include/navigator/preview.hpp` — `IPreviewProvider`
- `include/navigator/commands.hpp` — kayıt, keymap, palet
- `include/navigator/ui.hpp` — panel, sekme, layout, tema (çizim ayrıntısı gizli)
- `include/navigator/platform.hpp` — open, mime, trash, ipc, terminal
- `include/navigator/plugin.h` — C ABI

### 7.4 Koordinatörün birleştirme ve kapı kuralları

Fable 5.2 her fazda şunları uygular:

1. **Sıra:** sözleşmeler → bağımsız uygulamalar (paralel) → `app` bileşim / CMake kancaları → `qa` → `design-reviewer` (UI fazlarında).
2. **Birleştirme birimi:** küçük, derlenen commit’ler; her alt ajan kendi dalında veya net commit mesajıyla çalışır. Çakışan dosyayı sona bırakan integration geçişi yapar.
3. **Kapılar (hepsi yeşil olmadan faz bitmez):**
   - `cmake --build` (preset `dev`) sıfır uyarı hedefi (`-Wall -Wextra -Werror` aşamalı, en geç Faz 2’de zorunlu)
   - `ctest` ilgili testler
   - `clang-format --dry-run` ve `clang-tidy` (modül yolu)
   - Elle duman: başsız (`SDL_VIDEODRIVER=dummy` veya offscreen) veya xvfb ile “pencere açılır”
   - Sözleşme diff’i: kamu başlığı değiştiyse `docs/architecture.md` güncellenmiş olmalı
4. Alt ajan “sadece kendi modülüm” sınırını aşarsa koordinatör birleştirmeyi reddeder ve işi böler.
5. Test yazmayan özellik kabul edilmez (saf CMake/paket işleri hariç; orada en az bir paket şema denetimi gerekir).

---

## 8. Fazlar

Aşağıda 12 faz vardır (0–11). Her fazın sonunda **derlenen, çalışan bir yapım** bulunur. Promptlar İngilizce, yapıştırıma hazırdır.

Koordinatör promptu Fable 5.2’ye yapıştırılır. Fable 5.2 işi planlar, alt ajanlara böler, kapıları uygular. Alt ajan promptları Claude Code subagent’larına verilir. Tüm promptlar `CLAUDE.md` ve `docs/architecture.md` okumayı, test yazmayı, küçük commit’leri ve faz sonunda çalışan yapımı zorunlu kılar.

---

### Faz 0 — Temel: depo, sözleşmeler, CI, iskelet

**Hedef.** Boştan, derlenen bir C++ uygulaması: SDL3 penceresi, temiz kare döngüsü, FPS göstergesi, komut kuyruğu iskeleti, ajan tanımları, CI.

**Teslimatlar.** CMake preset’leri (`dev`, `release`, `tidy`); `vcpkg.json`; Catch2 ile `tests/smoke`; `.clang-format` / `.clang-tidy`; `.github/workflows/ci.yml` (format, tidy, test, Ubuntu); `CLAUDE.md`; `docs/architecture.md`; `include/navigator/*.hpp` iskeletleri; `.claude/agents/*.md`; `src/app` döngüsü (boş sahne + “Navigator” metni).

**Kabul.** `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` yeşil. CI aynı işi yapar. Pencere Wayland ve X11’de açılır (en azından geliştirici makinesinde not edilir; CI xvfb/X11). Başlıklar tek başına derlenir (`-Werror`).

**Paralel işler ve sıra.**

1. `contracts` — başlıklar + architecture (tek başına, önce).
2. Paralel: `packaging` (CMake/CI iskeleti), `ui` (SDL pencere + döngü), `qa` (Catch2 smoke, format dosyaları).
3. Integration: kök CMake, `main` kancası.
4. `design-reviewer` iskeleti (pencere, netlik) kısa geçer.

**Koordinatör promptu (Fable 5.2):**

```text
You are Fable 5.2, the orchestrator for Navigator (working title), an independent Linux file manager inspired by File Pilot. You are not an official File Pilot port. Do not copy File Pilot’s name, logo, or assets. Do not invent File Pilot features; follow docs/features.md and the roadmap when they exist.

Read CLAUDE.md (create it from the repository template if missing) and obey its coordinator rules: headers-first contracts, module ownership, no crossing src/ boundaries, command queue at frame end, tests, small commits, working build at the end of the phase.

Phase 0 goal: repository skeleton that compiles and runs a blank SDL3 window with a frame loop, a CommandQueue stub, Catch2 smoke tests, clang-format/tidy, and GitHub Actions CI.

Work order:
1) Create CLAUDE.md, docs/architecture.md, the include/navigator/*.hpp contract stubs, and .claude/agents/*.md (core-fs, platform, ui, search, preview, ops, commands, packaging, qa, design-reviewer, contracts).
2) Only after headers exist, dispatch parallel sub-agents with the Phase 0 sub-prompts: packaging (CMake/vcpkg/CI), ui (SDL3 window + loop), qa (Catch2 + format config). You own conflicting files (root CMakeLists.txt, src/app/main.cpp) in a final integration pass.
3) Enforce gates: dev preset build, ctest, clang-format dry-run, headers compile with -Werror, CI workflow present. Do not close the phase until all gates pass.
4) Commits should be small and logical. Do not push to a protected main branch if the project’s contribution rules forbid it; work on a branch.

Use C++20/23, CMake, SDL3, Catch2. No Qt, no gtkmm in the UI process. Keep dependencies listed in vcpkg.json.
```

**Alt ajan promptları:**

```text
[contracts] Read CLAUDE.md. Create include/navigator/{types,fs,watch,volume,index,search,ops,preview,commands,ui,platform}.hpp and include/navigator/plugin.h as documented stubs: types, Result/expected policy, thread-safety comments, no SDL/GIO leakage. Write docs/architecture.md describing layers (platform / renderer / program), frame-end command queue, module ownership, and merge rules. Add a header-only compile test. Do not implement src/ bodies beyond empty .cpp files if CMake needs them. Small commits. End with a note of every public type you introduced.
```

```text
[packaging] Read CLAUDE.md and existing include/navigator headers. Own CMakeLists.txt, CMakePresets.json, vcpkg.json, .github/workflows/ci.yml, packaging/ stubs. Configure C++20, Ninja presets dev/release/tidy, Catch2, SDL3, optional Dear ImGui, freetype, harfbuzz. CI on ubuntu-latest: checkout, cache vcpkg or apt packages if documented, build, ctest, clang-format dry-run. Do not rewrite UI logic. If you must edit root CMake, keep it minimal and document hooks for other modules. Small commits. Phase ends with a documented build command that works on a clean tree.
```

```text
[ui] Read CLAUDE.md and include/navigator/ui.hpp plus commands.hpp. Implement src/app and src/ui enough to open an SDL3 window on Wayland and X11, clear the frame, run a command queue (enqueue during the frame, execute after render), display a title and FPS, and exit cleanly on the window close / Quit command. Use a thin ImGui bootstrap only if it does not block a later custom file list. No filesystem browsing yet. Add a tiny unit or smoke test if feasible (headless/dummy driver). Do not edit include/navigator contracts except via a requested patch note. Small commits. Leave a running binary.
```

```text
[qa] Read CLAUDE.md. Add .clang-format (LLVM-based or the project’s documented style, 4-space indent, max column 100), .clang-tidy (bugprone, conservative modernize, no noisy churn), tests/smoke that ctest runs, and a short docs/qa-gates.md repeating the phase gates. Do not implement features. Small commits.
```

---

### Faz 1 — Çekirdek dosya sistemi ve izleme

**Hedef.** UI’siz, testli çekirdek + platform: dizin listeleme, metadata, sıralama, symlink, gizli/nokta dosya, birimler, inotify, bağlama/çıkarma.

**Teslimatlar.** `fs`, `watch`, `volume` uygulamaları; geçici dizin altın testleri; udev/udisks veya `/proc/mounts` + inotify fallback; Unicode yollar (Türkçe, NFD); izin reddi davranışı.

**Kabul.** 100k sentetik girdilik klasör testte sıralı liste üretilir (süre `docs` içine ölçülerek yazılır). inotify ile oluştur/sil/ad değiştir testi yeşil. Birim listesi en az yerel ext4/btrfs/nvme ve çıkarılabilir birimi gösterir (CI’da mock). UI henüz zorunlu değil; isteğe bağlı CLI duman: `navigator-ls PATH`.

**Sıra:** sözleşme sıkılaştırma (gerekirse) → paralel `core-fs`, `platform` → `qa` (yarış / inotify) → kısa integration (`navigator-ls`).

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md and docs/architecture.md. Phase 1: implement the filesystem core and Linux watchers, with tests, no full UI.

Headers-first: if include/navigator/fs.hpp, watch.hpp, or volume.hpp need changes, do that (or dispatch contracts) and land it before parallel implementation.

Dispatch in parallel only after contracts are stable:
- core-fs: listing, metadata, sort, hidden/dot/system policy, symlinks, permission errors, tests with temp dirs and Unicode names.
- platform: inotify watcher, volume enumeration, mount/unmount events, tests with mocks where root is required.

Then an integration pass: a small navigator-ls tool or example that prints a directory and stays in sync on a change. qa runs race-focused tests.

Gates: build, ctest (core + platform), clang-format/tidy on touched paths, architecture doc updated if APIs changed. Small commits, branch work, C++20/23, no Qt/gtkmm.
```

**Alt ajanlar:**

```text
[core-fs] Read CLAUDE.md and include/navigator/fs.hpp. Implement src/core listing and metadata without blocking the future UI thread (sync API first is OK if documented; heavy work must be off-loadable). Support sort keys (name, size, mtime, count), folders-first option, hidden and dotfile filters, symlink-as-file vs target (document the default: do not resolve links at listing time, matching File Pilot’s later behavior). Write Catch2 tests for Unicode names, empty dirs, dangling symlinks, and permission-denied entries. Do not invent a UI. Do not walk the whole disk yet. Small commits. Leave tests green.
```

```text
[platform] Read CLAUDE.md and include/navigator/watch.hpp, volume.hpp, platform.hpp. Implement inotify-based change events (create, delete, move, attrib) and volume list/eject/remount signals on Linux. Abstract behind the headers so tests can inject a fake watcher. Never require a real hardware unplug in CI. Do not touch src/ui. Small commits, tests green, document limitations (NFS, FUSE event gaps).
```

---

### Faz 2 — Tek panelli tarayıcı

**Hedef.** Gerçek bir gezgin iskeleti: sanal Details listesi, klavye ile gezinme, adres yolu, `xdg-open` ile açma, geri/ileri/yukarı.

**Teslimatlar.** Virtualized satır listesi (on binlerce girdi, sabit FPS hedefi); sütunlar Ad / Boyut / Tür / Tarih; seçim (oklar, `Ctrl+A`, `Ctrl` tık, dikdörtgen temel); durum çubuğu (sayı, seçili boyut); hata bildirimi (izin yok).

**Kabul.** 50k girdilik klasör kaydırılır, UI donmaz (geliştirici notu + mümkünse basit ölçüm). Enter/çift tık klasörde iner, dosyada `xdg-open` açar. Geri/ileri doğru. Wayland ve X11’de çalışır.

**Sıra:** `ui` liste widget’ı, `fs`’ye yalnızca başlık üzerinden bağlanır → `platform` (xdg-open) → `commands` (Activate, Back, Open) → `qa` / `design-reviewer`.

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md and docs/architecture.md. Phase 2: a usable single-pane details browser.

Do not let UI call std::filesystem directly. Wire through navigator::fs and navigator::platform::open_with_default_app.

Split:
- ui: virtualized details view, keyboard nav, address path display, status bar, frame-end commands.
- commands: Activate, Open, Back, Forward, Up, SelectAll, Refresh registered in the command registry (even if the palette UI is later).
- platform: xdg-open / gio launch, MIME query stub.
- qa + design-reviewer after the build runs.

Gates: build, tests, a manual or scripted smoke that opens a fixture directory, navigates into a subfolder, and goes back. 50k-file scroll must stay responsive; record a note in docs if you measure it. C++20/23, SDL3, small commits, working binary at the end.
```

**Alt ajanlar:**

```text
[ui] Read CLAUDE.md, include/navigator/ui.hpp, fs.hpp, commands.hpp. Implement a virtualized details list that only measures/draws visible rows. Keyboard: arrows, page, home/end, enter to activate, alt-up or the project’s Back/Up command. Mouse: click select, double-click activate, basic rubber-band if time allows else stub. Show name, size, type, mtime. Keep the command queue: input enqueues, execution happens at frame end. Do not implement tabs yet. Add a fixture-based test if possible (headless list model). Small commits. Working browse window.
```

```text
[commands] Read CLAUDE.md and include/navigator/commands.hpp. Register Phase 2 commands with stable ids and default bindings only when those bindings are documented in docs (Back, Forward, Up, Refresh, SelectAll, Activate). Provide key-to-command routing. Tests for registration and a simple sequence. Do not draw UI. Small commits.
```

```text
[platform] Implement open_with_default_app via xdg-open or GIO, plus a query for default mime app, behind include/navigator/platform.hpp. Tests can mock the launcher. Small commits.
```

```text
[design-reviewer] Review the Phase 2 UI for density, typography, contrast (light/dark later), and keyboard completeness. Write docs/review/phase-2.md. Do not push drive-by refactors; list required fixes vs later polish.
```

---

### Faz 3 — Sekmeler, bölünmüş paneller, yerleşim, oturum

**Hedef.** File Pilot’ın panel/sekme modeli: sürükleyerek böl, her panelde bağımsız sekmeler, layout kaydet/yükle, oturum geri yükleme. Tek örnek IPC henüz yok (Faz 10).

**Teslimatlar.** Tab çubuğu (yeni, kapat, orta tık, yeniden sırala); drop bölgeleri (sol/sağ/üst/alt); ayırıcı; layout JSON; kapanınca geri yükle; Close Left / reopen closed (Search Tabs temel).

**Kabul.** En az üç panelli yerleşim kaydedilip yeniden açılır. Sekme sürüklemek bölme üretir. `Ctrl+T` / `Ctrl+W` çalışır (docs’ta bağlanır).

**Sıra:** `contracts` (TabId, Layout) → `ui` (asıl iş) + `commands` paralel → `qa` (layout ser/de) → `design-reviewer`.

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 3: tabs, split panes, saved layouts, session restore — File Pilot style (drag a tab to split left/right/top/bottom; each pane has its own tabs).

Stabilize include/navigator/ui.hpp layout types first. Then dispatch ui (widgets + persistence) and commands (NewTab, CloseTab, DuplicateTab, CloseLeft, ReopenClosed, SaveLayout). qa tests JSON round-trip and “restore two panes with three tabs”. design-reviewer checks drop affordances and inactive-pane dimming.

Gates: build, tests, manual smoke of drag-to-split, session restore after restart. Do not start the sidebar yet. Small commits, C++20/23, working build.
```

**Alt ajanlar:**

```text
[ui] Implement tabs and split panes per include/navigator/ui.hpp and docs/architecture.md. Drag tab to an edge to split; drag to the tab bar to reorder or move between panes. Persist session and named layouts to the user config dir (XDG). Restore on startup. Dim inactive panes slightly. Middle-click closes a tab. Keep commands at frame end. Tests for layout tree split/unsplit. Small commits, running app.
```

```text
[commands] Add tab/layout commands and default keys documented in docs/keymap.md (Ctrl+T, Ctrl+W). For Ctrl+N: document the choice. Prefer File Pilot’s later “Duplicate Tab in New Window” only if a second window is already supported; otherwise bind New Window later and say so. Tests included. Small commits.
```

---

### Faz 4 — Kenar çubuğu, GoTo, adres çubuğu

**Hedef.** Recents, Bookmarks, Places, Volumes (kullanım çubuğu), GoTo paleti, breadcrumb, görünüm kilidi iskeleti.

**Teslimatlar.** Kenar çubuğu bölümleri (daraltılabilir); yer imi ekle/çıkar; çevrimdışı yer imi kalır; GoTo: yol, ortam değişkeni (`$HOME`, `~`, isteğe bağlı `%VAR%` uyumu), recents, otomatik tamamlama; breadcrumb tıklanır; `Ctrl+B` kenar çubuğu.

**Kabul.** GoTo ile `~/Downloads` ve `/` zıplanır. Çıkarılmış birime ait yer imi kaybolmaz, “offline” görünür. Breadcrumb ile yukarı inilir.

**Sıra:** `core-fs`/`platform` (volume usage) + `ui` + `commands` (GoTo, ToggleSidebar, Bookmark).

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 4: sidebar (Recents, Bookmarks, Places, Volumes with usage bars), GoTo with autocomplete, breadcrumbs, bookmark toggle, view-lock stub.

Headers first if needed (BookmarkStore, RecentsStore must survive offline volumes). Parallel: platform/core-fs for volume usage; ui for sidebar + GoTo + breadcrumbs; commands for GoTo, ToggleSidebar, ToggleBookmark.

Gates: tests for bookmark persistence when a path is missing; GoTo resolves ~ and env vars; build and a smoke of jumping to a path. Update docs. Small commits. Working build. C++20/23.
```

**Alt ajanlar:**

```text
[ui] Implement sidebar, GoTo modal, and breadcrumbs using only public headers. Volumes show a usage bar and percent. Recents can be highlighted without forcing sort-to-top (setting stub). Offline bookmarks remain visible. Ctrl+B toggles the sidebar via a command. Small commits, tests where pure logic exists, running UI.
```

```text
[platform / core-fs] Expose volume usage (bytes used/total) and a stable volume id so bookmarks can stay offline. Tests with a fake volume table. Do not draw UI. Small commits.
```

```text
[commands] Register GoTo, ToggleSidebar, ToggleBookmark, and path-copy commands. Tests for resolution of ~, $VAR, and invalid paths. Small commits.
```

---

### Faz 5 — Arama, filtre, indeks

**Hedef.** Anlık klasör filtresi + hiyerarşiyi düzleştiren geniş arama (`Ctrl+E`) + fuzzy + uzantı süzgeci. Ardından arka plan ad indeksi (yerel birimler).

**Teslimatlar.** Filtre kutusu (odak, anlık); flattened enumerator (iptal edilebilir, izin hatalarını atlar); fuzzy sıralama; tür popup; indeks: artımlı walk + inotify; ilerleme göstergesi; aramayı iptal.

**Kabul.** Geçerli klasör filtresi anlık hissi verir. Birkaç yüz bin girdilik test ağacında geniş arama iptal edilebilir ve UI donmaz. İndeks yeniden açılışta soğuk taramadan hızlıdır (ölçümü docs’a yaz). Joker ileride; şimdilik parçalı ad + fuzzy.

**Sıra:** `search` asıl; `core-fs` enumerator; `ui` kutu ve sonuç listesi; `qa` doğruluk testleri.

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 5: instant current-folder filter, flatten/expand search (Ctrl+E) with fuzzy name match and extension filters, plus a cancellable background name index for local volumes.

Do not block the UI thread on a full-tree walk. Dispatch search (engine + index), core-fs (if incremental enumeration API is missing), and ui (filter box, types popup, progress). qa owns correctness tests (unicode, cancelled search, permission-denied skips, hidden-file policy).

Gates: unit tests, a benchmark note, working UI filter + expand, build green. Content search (ripgrep-like) is Phase 10, not now. C++20/23, small commits.
```

**Alt ajanlar:**

```text
[search] Implement filter, fuzzy, flatten, and a persistent name index behind include/navigator/search.hpp and index.hpp. Support cancel tokens, extension filters, and skipping unreadable dirs. Tests with a generated tree. Document file format of the index. Do not draw widgets. Small commits.
```

```text
[ui] Add the filter box (most commands still work while it is focused, then apply on blur — File Pilot behavior). Ctrl+E expands to flattened/index search. Types popup toggles extensions. Show progress and allow Escape to cancel. Small commits, running app.
```

```text
[qa] Gold tests for search ranking, cancel, and hidden-file exclusion in flattened mode (do not include children of hidden/system dirs, matching File Pilot’s v0.5 bugfix). Small commits.
```

---

### Faz 6 — Inspector, küçük resimler, önizleme

**Hedef.** `Space` ile Inspector: metin, görüntü (EXIF yön, sığdır/1:1), klasör içeriği. FreeDesktop thumbnail spec. Önbellek boyutu ayarı.

**Teslimatlar.** `IPreviewProvider` eklenti noktası; metin (büyük satırlarda sanal kaydırma — FP’de uzun satırda FPS düşmüştü, tekrarlanmasın); görüntü; klasör önizleme; thumbnail arka plan kuyruğu; animasyon kapalıyken fade yok.

**Kabul.** PNG/JPEG/WebP, UTF-8 metin, bir klasör önizlenir. Thumbnail’ler `~/.cache/thumbnails` ile uyumlu. 100 MP uç görüntü çökmez (downscale). PDF/SVG bu fazda isteğe bağlı; yoksa provider arayüzü ve “yakında” durumu.

**Sıra:** `preview` + `ui` (inspector yerleştirme sağ/alt) + `platform` (thumbnail path) → `design-reviewer`.

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 6: Inspector (Space) for text, images (EXIF orientation, fit/1:1), and folder contents; Freedesktop thumbnail spec; asset cache size option.

Dispatch preview (decoders, cache, providers), ui (inspector pane, lock, metadata), platform (thumbnail cache paths). Optional PDF/SVG providers if time remains; otherwise leave interfaces and a clear fallback. Guard extreme image sizes. Virtualize long text lines.

Gates: tests for cache keys and EXIF orientation, build, smoke on fixture files, no UI freeze on a large text file. Small commits, C++20/23, working build.
```

**Alt ajanlar:**

```text
[preview] Implement providers and a bounded cache (bytes configurable). Generate and read Freedesktop thumbnails. Off-thread decode, main-thread upload. Tests for cache hit/miss and a tiny PNG fixture. Do not implement window chrome. Small commits.
```

```text
[ui] Inspector pane toggled by the Inspect command (Space; also map Alt+KP_Enter). Position right or bottom from window size, overridable. Folder preview shows inner listing without navigating away. Respect “animations off”. Small commits, running app.
```

---

### Faz 7 — Dosya işlemleri, çöp, geri al, sürükle-bırak

**Hedef.** Kopyala/taşı/sil (Trash), çakışma diyaloğu, ilerleme, Undo/Redo, SDL3 DnD (iç paneller, dış uygulamalar, arşivden bırakma temel).

**Teslimatlar.** `ops` kuyruğu (arka plan, iptal); XDG Trash + Geri Yükle; Undo yığını (ad, taşı, çöp; kalıcı silme için uyarı); aynı ada çakışma (atla, değiştir, kopya adı); panelden panele sürükleme; dışarı `text/uri-list`.

**Kabul.** İki panel arasında taşıma Undo ile geri gelir. Silinen öge çöp kutusundan geri yüklenir. Wayland’de en az bir dış hedefe (Nautilus veya masaüstü) sürükleme denenir ve sonuç docs’a yazılır (portal kısıtı varsa dürüstçe).

**Sıra:** `ops` + `platform` (trash) → `ui` (ilerleme, çakışma, DnD) → `commands` → `qa` (işlem atomikliği).

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 7: copy/move/delete-to-trash, conflict and progress UI, Undo/Redo, drag-and-drop between panes and to/from other apps.

This is safety-critical. Tests first for ops on temp trees (including undo). Never run destructive tests outside fixtures. Headers-first for ops and trash.

Dispatch ops (engine + undo log), platform (XDG Trash), ui (progress, conflicts, DnD), commands (Copy, Cut, Paste, Trash, Undo, Redo, CopyPath). qa adds conflict and partial-failure tests.

Gates: all ops tests green, manual DnD note, build, working app. Small commits. C++20/23.
```

**Alt ajanlar:**

```text
[ops] Implement copy, move, rename, mkdir, trash, restore, and an undo/redo stack with recorded inverse operations. Conflict policy documented. Jobs cancellable. Catch2 tests on temporary directories only. Small commits.
```

```text
[platform] XDG Trash spec (home trash and, where applicable, $topdir/.Trash-$uid). Do not silently fallback to rm unless a documented “Delete permanently” command is used. Tests with a fake home. Small commits.
```

```text
[ui] Progress UI, conflict dialog, and SDL3 drag-and-drop (internal first, then text/uri-list). Auto-scroll while dragging near edges. Frame-end commands only. Small commits, running app.
```

---

### Faz 8 — Komut paleti, aranabilir bağlam menüsü, tuş takımı

**Hedef.** Her eylem palette ve yeniden bağlanabilir. Tuş dizileri, takma adlar, numpad. Bağlam menüsü aranır ve pin’lenir. Linux MIME / `.desktop` eylemleri (Open With).

**Teslimatlar.** `Ctrl+Shift+P` palet; keymap dosyası (JSON); zincir kısayollar (timeout dokümante); menü arama kutusu; pin listesi; Open With (gio).

**Kabul.** Paletten “New Folder”, “Toggle Sidebar”, “GoTo” bulunur ve çalışır. Kullanıcı `Ctrl+E`’yi yeniden bağlayabilir, yeniden açılışta kalır. Menüde yazarak “Trash” bulunur.

**Sıra:** `commands` asıl; `ui` palet/menü; `platform` Open With; `qa` keymap.

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 8: command palette (Ctrl+Shift+P), fully remappable hotkeys including key sequences, aliases, and numpad; searchable context menu with pinned actions; Open With from MIME/desktop files.

Dispatch commands (registry, keymap persistence, sequences), ui (palette and menu search/pin), platform (Open With list). qa tests keymap load/save and sequence matching.

Gates: tests, build, smoke: rebind a key, restart, binding persists. Document defaults in docs/keymap.md. Small commits. C++20/23. Working build.
```

**Alt ajanlar:**

```text
[commands] Implement registry sections, aliases, sequence matching, and keymap file I/O (unknown fields ignored, File Pilot-like lenient load). Tests for chains and conflicts. Small commits.
```

```text
[ui] Palette UI with type-ahead; context menu with search field and pin-to-top. Show shortcuts in the UI. Do not hardcode actions that belong in the registry. Small commits, running app.
```

```text
[platform] Enumerate “Open With” handlers via GIO/MIME. Tests with a mock. Small commits.
```

---

### Faz 9 — Görünümler, toplu adlandırma, tema

**Hedef.** Details / ızgara-ikon (zoom), List/columns, Tree, Group by, hiyerarşik görünüm ayarları, etkileşimli toplu adlandırma, tema ve yoğunluk.

**Teslimatlar.** Zoom bir eksen (details ↔ büyük ikon); group by tarih/tür/boyut; per-folder view lock gerçeklenir; batch rename önizleme (tarih, benzersiz id, tek satır yapıştır); temalar (açık/koyu/özel JSON); yazı tipi, aralık, animasyon anahtarı, satır zebra, köşe yuvarlama.

**Kabul.** Aynı klasör ızgara ve tree arasında durumunu korur. 200 dosyalık toplu ad önizlemesi çakışmayı gösterir, uygulayınca Undo ile geri alınır. Animasyon kapalıyken fade/chevron yok.

**Sıra:** `ui` görünümler + tema; toplu ad mantığı `ops` altında; `design-reviewer` yoğun.

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 9: view modes (details, icon grid with live zoom, columns/list, tree, group-by), hierarchical per-folder view settings, interactive batch rename (dates, unique ids, paste-one-line-to-many), themes (light/dark/custom JSON), typography, spacing, animations toggle.

Dispatch ui (views + theme), ops (batch rename engine + undo), commands (view and rename commands), design-reviewer (visual density worthy of a professional file manager, not default ImGui).

Gates: rename dry-run tests (collisions, Unicode), theme JSON load, build, working zoom and tree. Small commits. C++20/23.
```

**Alt ajanlar:**

```text
[ui] Implement view modes and theme system per headers. Ctrl+wheel or a footer % slider zooms. Tree and group-by must stay virtualized. Respect animations-off. Persist view settings per folder and as a hierarchy/default, File Pilot 0.8.1 style. Small commits, running app.
```

```text
[ops] Batch rename engine: preview table, unique ids, date tokens, one-line paste to many, collision detection, apply with undo. Catch2 tests only on fixtures. Small commits.
```

```text
[design-reviewer] Review typography, spacing, icon alignment, light and dark contrast, and “does this feel snappy and dense rather than a toolkit demo?”. Write docs/review/phase-9.md with blocking vs optional items. Do not implement unless asked to fix blockers in a follow-up.
```

---

### Faz 10 — Linux ekstraları: GVfs, git, uçbirim, arşiv, eklenti, IPC

**Hedef.** “Daha fazlası” listesinin ilk üretim dilimi: GVfs (MTP/SMB/sftp), git overlay, uçbirim aç, arşivlere göz at, eklenti C ABI iskeleti, tek örnek IPC, içerik araması, Miller columns temel, CLI. Varsayılan gezgin kaydı Faz 11’de paketle birlikte gelir.

**Teslimatlar.** `gio` mount’ları kenar çubuğunda; MTP birimi listelenir; `navigator --new-tab PATH` çalışan örneğe düşer; git durumu ikonları; “Open Terminal Here”; zip/tar libarchive ile iç gezinme + DnD çıkarma; `include/navigator/plugin.h` + örnek eklenti; içerik araması; CLI.

**Kabul.** Bir SMB veya MTP yolu (ortam varsa) listelenir; yoksa GVfs test mock’u ve dokümante el speği. İkinci `navigator PATH` çağrısı yeni süreç yığını açmaz, sekmeye ekler. Örnek eklenti yüklenir. İçerik araması iptal edilebilir.

**Sıra (geniş faz — Fable 5.2 alt dilimlere bölebilir):** sözleşmeler → paralel `platform` (gvfs, ipc, terminal), `ops` (archive), `search` (content), `ui` (miller, git badges), `plugins` → `qa`.

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 10 is large: split into clearly owned slices and do not let agents edit the same files.

Slices (parallel after headers):
- platform: GVfs/MTP/SMB/SFTP mounts, Open Terminal Here, single-instance IPC, CLI argument forwarding.
- search: optional content search (ripgrep-like), cancellable.
- ops: libarchive browse and extract-via-open, drag out of archives.
- ui: git status overlays, Miller columns v1, mount entries in the sidebar.
- plugins: C ABI loader + sample plugin; process isolation is optional v1 (document risk); prefer in-process with a tight API and no hook during FS write unless reviewed.
- qa: integration tests and a manual matrix (Wayland, X11, no-gvfs build).

Gates: CLI --help, second-process tab adoption test, archive fixture test, plugin sample loads, build. If GVfs is an optional dependency, the app must still run without it. Small commits. C++20/23. Working build. Do not start packaging release pipelines here beyond CLI install path hints.
```

**Alt ajanlar:**

```text
[platform] GVfs wrapper behind include/navigator/platform.hpp, terminal launcher (xdg, then $TERMINAL, then x-terminal-emulator), Unix socket or D-Bus IPC for single-instance --new-tab/--select. CLI parser in src/app or src/platform as contracted. Tests for IPC with two processes in CI if possible. Small commits.
```

```text
[search] Content search API and implementation (respect binary skip, size cap, cancel). Do not block UI. Tests with a fixture tree. Small commits.
```

```text
[ops] Archive listing and extract using libarchive; treat an opened archive as a virtual folder if the fs contract allows a provider, otherwise a dedicated ArchiveFS implementing the same listing interface. Tests with a checked-in zip/tar fixture. Small commits.
```

```text
[ui] Git badges (read-only, via libgit2 or `git status --porcelain` cached). Miller columns basic navigation. Sidebar mount section. Small commits, running app.
```

```text
[plugins / contracts] Freeze include/navigator/plugin.h (C ABI): name, commands, preview, column, overlay. Loader, version check, sample plugin. Tests load the sample. Document the API. Small commits.
```

---

### Faz 11 — Paketleme, varsayılan gezgin, sürüm

**Hedef.** Kullanıcının Linux Mint / GNOME makinesine kurulabilir sürüm: `.deb`, AppImage, Flatpak; `xdg-mime` / `.desktop`; sürüm numarası; imza/checksum; CI “release” iş akışı (etiketle yayın; korumalı `main` kurallarına uy).

**Teslimatlar.** `packaging/deb`, `packaging/appimage`, `packaging/flatpak`; `navigator.desktop` (`inode/directory`, `MimeType`); isteğe bağlı “varsayılan yap” betiği (kullanıcı onaylı); TR/EN çeviri taraması; kısa `README` ve `docs/manual` iskeleti; kilitlenmiş performans notu (açılış, 50k liste, arama).

**Kabul.** Temiz Ubuntu/Mint benzeri bir ortamda `.deb` kurulur, menüde görünür, `navigator` PATH’tedir, bir klasöre varsayılan uygulama olarak atanabilir (talimatlı). AppImage tek dosya çalışır (FUSE). Flatpak dosya izinleri (home + media + portal) belgelenir. CI etiketle üç bakıyı da üretir (imza anahtarı yoksa checksum yeter, sırrı repoya koyma).

**Sıra:** `packaging` asıl; i18n kalanları; `qa` kurulum matrisi; `design-reviewer` son geçiş.

**Koordinatör promptu:**

```text
You are Fable 5.2, orchestrator for Navigator. Read CLAUDE.md. Phase 11: packaging and release. Produce .deb (CPack or a documented path), AppImage, and a Flatpak manifest; install a navigator.desktop with inode/directory; document how the user sets Navigator as the default file manager on GNOME/Mint; finish tr/en i18n pass; add a release GitHub Actions workflow on tags that builds artifacts and publishes checksums. Never store signing secrets in the repo.

Dispatch packaging (main), qa (install smoke instructions and any automated package lint), and design-reviewer (final visual + about dialog, no File Pilot trademark). Integration: version in one header/CMake place, changelog.

Gates: CI release workflow dry-run or documented, .deb installs on a clean tree or container, AppImage runs, translations have no obvious missing keys, README is accurate. Small commits. Working packaged build. C++20/23.
```

**Alt ajanlar:**

```text
[packaging] Own packaging/**, desktop file, metainfo/appstream if needed, CPack, linuxdeploy, Flatpak. Prefer linking .deb against distro SDL3/freetype/harfbuzz/glib when versions allow; bundle in AppImage/Flatpak. Add a tag-based CI workflow with checksums. Do not change core features. Small commits. Document install and default-app commands for the user (xdg-mime default).
```

```text
[qa] Write a release checklist (Wayland, X11, trash, MTP if present, single-instance, search, package install). Automate whatever you can in containers. Small commits.
```

```text
[design-reviewer] Final pass: about box, icons, desktop name (not “File Pilot”), contrast, empty states. Write docs/review/phase-11.md. Small commits only if fixing blockers you are asked to fix.
```

---

## 9. Önerilen `.claude/agents/*.md` tanımları

Faz 0 bunları depoya koyar. Claude Code’un ajan biçimi YAML ön yüz + sistem metnidir. Fable 5.2 bu dosyaları alt ajanlara rol olarak bağlar. Araç adlarını uydurmayın; ortamın verdiği araçları kullanın.

### `contracts.md`

```markdown
---
name: contracts
description: Owns public headers under include/navigator and docs/architecture.md. Use before any new module or when an API must change. Headers-first, no feature creep in src/.
---
You own include/navigator/** and docs/architecture.md. Prefer incomplete but compilable headers over silent API invention in implementation PRs. No SDL, GIO, or ImGui types in public headers. Document thread-safety and error policy. Do not implement UI or package scripts unless writing a compile test for a header. Follow CLAUDE.md.
```

### `core-fs.md`

```markdown
---
name: core-fs
description: Filesystem listing, metadata, sort, path policy, core tests. Use for src/core and include/navigator/fs.hpp implementations.
---
You own src/core/** and tests/core/**. Implement include/navigator/fs.hpp only; request contract changes instead of editing headers casually. Never call UI. Prefer deterministic Catch2 tests on temporary directories. Unicode paths are required. Follow CLAUDE.md, small commits, keep tests green.
```

### `platform.md`

```markdown
---
name: platform
description: Linux integration — inotify, volumes, XDG, GVfs, trash, IPC, MIME, terminal. Owns src/platform.
---
You own src/platform/**. Wrap GIO/SDL/OS calls behind include/navigator/platform.hpp, watch.hpp, volume.hpp. Optional GVfs: the app must compile without it. Mock what CI cannot do. Do not draw widgets. Follow CLAUDE.md.
```

### `ui.md`

```markdown
---
name: ui
description: SDL3 window, immediate-mode UI, tabs, panes, views, themes. Owns src/ui and typically src/app loop wiring when assigned.
---
You own src/ui/** and, when the coordinator assigns it, src/app/**. Draw only through the project renderer. Enqueue commands; do not mutate filesystem directly. Virtualize all long lists. Wayland and X11 both matter. Do not ship default Dear ImGui styling as the product look. Follow CLAUDE.md and design-reviewer notes.
```

### `search.md`

```markdown
---
name: search
description: Filter, flatten, fuzzy, name index, later content search. Owns src/search.
---
You own src/search/**. All long work is cancellable and off the UI thread. Tests on generated trees. Do not implement window chrome. Follow CLAUDE.md.
```

### `preview.md`

```markdown
---
name: preview
description: Inspector providers, thumbnails, decoders, caches. Owns src/preview.
---
You own src/preview/**. Implement IPreviewProvider. Decode off-thread, bound memory, Freedesktop thumbnail paths. Never crash on corrupt or huge files. Follow CLAUDE.md.
```

### `ops.md`

```markdown
---
name: ops
description: Copy, move, trash, undo/redo, batch rename, archive extract. Owns src/ops.
---
You own src/ops/**. Destructive operations run only on fixtures in tests. Every mutating action that can undo must record an inverse. Follow the trash spec; no silent rm. Follow CLAUDE.md.
```

### `commands.md`

```markdown
---
name: commands
description: Command registry, keymap, sequences, aliases, palette data. Owns src/commands.
---
You own src/commands/**. Every user action has a stable CommandId. Keymap load is lenient. Tests for conflicts and sequences. No renderer calls. Follow CLAUDE.md.
```

### `packaging.md`

```markdown
---
name: packaging
description: CMake, vcpkg, CI, .deb, AppImage, Flatpak, desktop/appstream. Owns packaging/ and CI workflows.
---
You own packaging/**, CI workflows, and CMake glue that does not belong to a feature module. Do not implement product features. Do not commit secrets. Prefer distro libraries for .deb. Follow CLAUDE.md.
```

### `qa.md`

```markdown
---
name: qa
description: Tests, format/tidy gates, checklists, bug reproduction. Extends tests/** and docs/qa*.
---
You add and tighten tests, reproduce bugs, and run gates. You do not sneak features. If a test requires an API change, file it for contracts. Follow CLAUDE.md.
```

### `design-reviewer.md`

```markdown
---
name: design-reviewer
description: Visual and interaction review. Writes docs/review/*; does not merge feature code unless asked to fix blockers.
---
Review density, typography, contrast, keyboard paths, empty states, and whether the UI feels immediate. Compare intent to File Pilot’s documented UX, not its assets. Navigator must not use File Pilot branding. Write a short review note with blocking vs optional items.
```

---

## 10. CLAUDE.md taslağı

Aşağıdaki metin depo köküne `CLAUDE.md` olarak konur (İngilizce). Fable 5.2 ve tüm Claude Code alt ajanları her oturumda bunu okur.

```markdown
# CLAUDE.md — Navigator

Navigator is an independent Linux file manager inspired by File Pilot’s documented UX. It is not an official port. Do not copy File Pilot’s name, logo, artwork, or binaries. Do not reverse engineer File Pilot. Do not invent features and claim File Pilot has them; see docs/features.md.

## Orchestration

The coordinator model is Fable 5.2. It plans work, splits it across Claude Code sub-agents, and enforces contracts, merge order, and gates. Sub-agents do not freelance outside their module. If you are Fable 5.2, do not implement an entire phase in one mixed diff when it can be split. If you are a sub-agent, read this file and your `.claude/agents/<role>.md`.

## Stack

- C++20, C++23 where available
- CMake + Ninja presets, vcpkg for dev, Catch2, clang-format, clang-tidy
- SDL3 + custom immediate-mode UI (Dear ImGui is bootstrap only, not the product look)
- SDL_GPU (Vulkan) with OpenGL 3.3 fallback
- FreeType + HarfBuzz for text
- No Qt, no gtkmm in the UI

## Architecture

Layers: platform, renderer, program (see docs/architecture.md). Most logic is platform-agnostic. The UI is immediate-mode. User actions enqueue `CommandId`s and run at frame end. Do not mutate program state mid-draw.

## Headers first

Public API lives in `include/navigator/*.hpp` (C ABI in `plugin.h`). Change contracts before parallel implementation. Public headers must not include SDL, GIO, or ImGui. One error policy (`navigator::Result` / `std::expected`). Document thread-safety.

## Module ownership

| Role        | Owns                          |
|-------------|-------------------------------|
| contracts   | include/navigator, docs/architecture.md |
| core-fs     | src/core, tests/core          |
| platform    | src/platform                  |
| ui          | src/ui, src/app when assigned |
| search      | src/search                    |
| preview     | src/preview                   |
| ops         | src/ops                       |
| commands    | src/commands                  |
| packaging   | packaging/, CI, CMake glue    |
| qa          | tests expansion, gates        |
| design-reviewer | docs/review only          |

Do not edit another role’s `src/` tree. Root `CMakeLists.txt` and `src/app/main.cpp` are integration-owned: write a note in `docs/integration-notes.md` or leave them to the coordinator’s integration pass.

## Coordinator rules (Fable 5.2)

1. Confirm phase goal, deliverables, and acceptance criteria from the roadmap.
2. Land or freeze headers before dispatching parallel implementation agents.
3. Give each agent a closed file set and a self-contained prompt.
4. Merge order: contracts → parallel modules → integration (CMake/main) → qa → design-reviewer (when UI changes).
5. Reject a diff that crosses ownership or ships a feature without tests.
6. Phase exit gates (all required):
   - `cmake --build --preset dev` succeeds
   - `ctest --preset dev` (affected tests) green
   - clang-format dry-run and clang-tidy on touched files
   - public header changes update docs/architecture.md
   - a working binary demonstrates the phase goal
7. Prefer small, logical commits on a branch. Do not force-push shared branches. Do not commit secrets.
8. Performance bar: the UI thread must stay interactive during listing, search, decode, and copy. Heavy work goes off-thread and is cancellable.
9. Safety: tests that create, move, or delete files use temporary fixture directories only.

## Commands and keymap

Every user-facing action has a stable `CommandId` and can appear in the command palette. Default bindings are documented in docs/keymap.md. Do not hardcode keys in widgets except as a last-resort fallback that still goes through the registry.

## i18n and platforms

UI strings are translatable from the start (English and Turkish). Primary targets: Linux Mint and GNOME, Wayland first, X11 second.

## What “done” means for a task

- Builds and, where relevant, runs
- Tests exist and pass
- Ownership respected
- Docs updated if contracts changed
- Short summary of what landed and what was deferred
```

---

## 11. Fable 5.2 çalışma düzeni (kısa)

1. Kullanıcı (veya üst ajan) ilgili **faz koordinatör promptunu** Fable 5.2’ye yapıştırır.
2. Fable 5.2 `CLAUDE.md` + `docs/architecture.md` okur, gerekirse sözleşmeleri günceller, işi alt ajan tanımlarına göre böler.
3. Birbirine bağımlı olmayan ajanlar **paralel** çalışır; çakışan dosyalar integration geçişine bırakılır.
4. Kapılar yeşil değilse faz kapanmaz.
5. Kullanıcı incelemesi (özellikle UI fazlarında `design-reviewer` notu) sonra sonraki faza geçilir. Yayın (`main` / etiket / GitHub Release) proje sahibinin kurallarına uyar.

Bu belge bir uygulama kodu üretmez; Claude Code alt ajanlarına verilecek işin sözleşmesidir. Özellik eklerken Bölüm 3–5’teki kaynak notlarına uyulur, uydurma File Pilot özelliği yazılmaz.
