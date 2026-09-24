# 🏁 Suzuki Swift 1.3 8V (G13BA) - ESP32 Show Tuning & Pops and Bangs

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange.svg)](https://platformio.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

ESP32 alapú, Wi-Fi Web Dashboarddal vezérelhető motorsport show-tuning és gyújtáselvételes fordulatszám-szabályzó rendszer Suzuki Swift 1.3 8V (G13BA motorkód, osztófejes gyújtás) típushoz.

---

## 📲 Firmware Frissítés (a legegyszerűbbtől)

Ha a vezérlő már be van szerelve a kocsiba, nem kell kiszerelned! Az autó Wi-Fi hálózata:
- **SSID:** `Swift-PopsAndBangs` • **Jelszó:** `swift123` • **Dashboard:** `http://192.168.4.1`

Minden módszerre igaz:
- 🛑 Frissíteni csak **álló autóval, alapjáraton vagy leállított motorral** lehet (max. **1500 RPM**), különben a vezérlő elutasítja.
- 🛡️ A flash írása alatt a szikraelvétel teljesen tiltva van (a gyári gyújtás 100%-ban működik).
- ✅ Sérült vagy félbeszakadt letöltés nem kerül telepítésre (MD5 ellenőrzés) – ilyenkor a régi firmware fut tovább.
- ↩️ Automatikus visszaállás: az új firmware-t a vezérlő csak akkor tartja meg, ha az első indulás után ~10 mp-ig rendben fut a Wi-Fi-vel (vagy betölt a dashboard). Ha közben lefagy vagy újraindul, a következő indításkor magától a régi verzió indul vissza – nem kell kiszerelni.
- ℹ️ Az 1. és 2. mód az új firmware része: a régebbi firmware-ről először a 3. vagy a 4. móddal frissíts.

### 1. ⚡ Egy koppintás a telefon mobilnetén keresztül (ajánlott)
1. Kapcsold be a telefonon a **mobilnetet**, és csatlakozz az autó Wi-Fi-jére.
2. Nyisd meg a dashboardot (`http://192.168.4.1`) és görgess a frissítés kártyához.
3. A telefon a mobilneten letölti a GitHubról a legújabb `firmware.bin`-t, majd egy koppintásra átküldi a vezérlőnek (~15–30 mp, automatikus újraindulás).

> Ha a telefon az autó Wi-Fi-jén nem éri el az internetet (egyes telefonok ilyenkor minden forgalmat a Wi-Fi-re küldenek), használd a 2. vagy a 3. módot.

### 2. 📡 A vezérlő maga tölti le (otthoni Wi-Fi vagy telefonos hotspot)
1. A dashboard frissítés kártyáján add meg **egyszer** egy internetes Wi-Fi nevét és jelszavát (otthoni router vagy egy telefon hotspotja). A vezérlő elmenti (a jelszót nem lehet visszaolvasni).
2. **Frissítés keresése:** a vezérlő rácsatlakozik (max. 20 mp), elolvassa a GitHubon a `version.json`-t, és kiírja, van-e újabb verzió.
3. **Telepítés:** a vezérlő közvetlenül a GitHubról (titkosított, tanúsítvánnyal ellenőrzött HTTPS kapcsolaton) letölti és beírja a `firmware.bin`-t, majd újraindul.

- Az autó Wi-Fi-je közben is él, de a vezérlő átáll az internetes Wi-Fi csatornájára, ezért a telefon pár másodpercre lecsatlakozhat – csatlakozz vissza, a folyamat a háttérben fut tovább.
- **Automatikus ellenőrzés indításkor** (opcionális, alapból ki): ha be van kapcsolva és van mentett Wi-Fi, bekapcsolás után egyszer megnézi a GitHubot (magától nem telepít), így a dashboard jelzi, ha új verzió érhető el.
- Telefonos hotspotnál: ha a telefonod nem tud egyszerre hotspot lenni és az autó Wi-Fi-jén maradni, a hotspotot adja egy másik telefon (vagy használd az otthoni Wi-Fit).

### 3. 📁 Kézi feltöltés (.bin fájl)
1. Töltsd le a telefonodra: 👉 **[📥 firmware.bin LETÖLTÉSE (Közvetlen link)](https://raw.githubusercontent.com/eng4t3/SwiftPopsAndBangs/main/firmware.bin)**
2. Csatlakozz az autó Wi-Fi-jére, nyisd meg a `http://192.168.4.1`-et, a frissítés kártyán válaszd ki a letöltött fájlt, és indítsd a telepítést (~15 mp, automatikus újraindulás).
- Tartalék feltöltő oldal (JavaScript nélkül is működik): `http://192.168.4.1/update`

### 4. 💻 Laptopról, vezeték nélkül (fejlesztőknek)
Csatlakoztasd a laptopot az autó Wi-Fi-jére (`Swift-PopsAndBangs`), majd:
```powershell
pio run -e esp32dev_wifi -t upload
```
Lefordítja a firmware-t, és a Windows 10/11 beépített `curl`-jével feltölti a `http://192.168.4.1/update` címre, MD5 ellenőrzéssel. Más IP-cím esetén: `--upload-port 192.168.x.y`.

---

## ✨ Főbb Funkciók

1. **🏁 Rajtautomatika / 2-Step (Launch Control):**
   - **Hands-Free mód:** Egyetlen érintéssel élesíthető (10 mp készenlét). Kuplung be, padlógáz $\to$ a beállított értéken (pl. 3800 RPM) dadog és lángol. A kuplung hirtelen felengedésekor (hajtáslánc terhelési beesés érzékelésekor) azonnal és automatikusan feloldja a tiltást a tökéletes kilövéshez!
   - **Show mód:** Állóhelyzeti durrogtatás és tűzköpés haveroknak a gomb nyomva tartásával.
   - **Hardveres kapcsoló (GPIO 23):** Kézifékre vagy kuplungpedál mikrokapcsolóra köthető.

2. **⚡ Redline Rev Limiter (Maximális tiltás):**
   - Gyári lassú üzemanyag-elvétel helyett villámgyors szikraelvétel (Bee*R limiter stílusú géppuskasorozat).

3. **💥 Overrun Decel Pops (Motorfék durrogás):**
   - Gázelvételkor a motorféküzem alatt rövid szikravágásokat iktat be.
   - **250 ms gyorsulási zár:** Gyorsítás közben nem vág bele a gyújtásba, csak valós gázelvételkor és tartós fordulatszám-esésnél aktiválódik.

4. **🎵 5 Állítható Kipufogó Hangzás / Mintázat:**
   - `0` - **Kemény tiltás (Hard Cut):** 100% vágás (klasszikus Bee*R stílus).
   - `1` - **Lángcsóva (Flames):** 3 vágás / 1 szikra a hatalmas lángokhoz.
   - `2` - **Durrogás (Gunfire):** Ritmikus lövések és ropogás.
   - `3` - **AK-47 Sorozat:** Ultramagas frekvenciájú staccato géppuskahang.
   - `4` - **💣 Ágyúlövés / Bomba:** 1.6 másodperces szünet padlógáznál (a kipufogó megtelik benzinnel), majd hirtelen szikravisszaadás $\to$ gigantikus dörrenés és tűzgolyó!

5. **🔥 Ghost Cam™ / V8 Alapjárati Dadogás:**
   - Alapjáraton (650–1250 RPM) ritmikusan szikrát vesz el (forgó 5-ös ciklus: 4 henger gyújt, 1 kimarad).
   - Amerikai nagytengelyes V8 drag-motorok lusta, agresszív dadogását produkálja.
   - Gázadásra (1250 RPM felett) automatikusan és észrevétlenül kikapcsol.

6. **📊 Valós idejű Telemetria & Neon Web Dashboard:**
   - 30 FPS sebességű kétirányú WebSocket kapcsolat.
   - Analóg íves fordulatszámmérő mutatóval és digitális kijelzővel.
   - Csúcsérték-memória (Peak RPM).
   - Beépített részletes súgó és funkcióleírások modális ablakban.

7. **🛡️ Biztonsági Védelem & Memória:**
   - **Trafó zavarszűrés:** 4000 µs hardveres zajzár és 4-ütemű görgetett átlagoló puffer a distributor holtjáték és zavarok ellen.
   - **Szikraelvétel fordulatszám-kompenzáció:** Nem esik be a mutatott RPM szikravágás alatt sem.
   - **Leállásvédelem (Anti-Flood):** Időkorlát a gyertyák beköpése ellen (állítható, vagy kikapcsolható).
   - **Fail-safe tranzisztor logika:** Ha az ESP32 áramtalanítva van vagy újraindul, a tranzisztor lezár $\to$ a gyári gyújtás 100%-ban működik.
   - **NVS Flash memória:** Minden beállítás automatikusan megőrződik gyújtáslevétel után is.

---

## 🔌 Bekötési Vázlat (Hardware Pinout)

| Funkció | ESP32 Pin | Hardver modul / Csatlakozás |
| :--- | :--- | :--- |
| **Fordulatszám bemenet (Tach In)** | `GPIO 18` | PC817 optocsatoló kimenet. Bemenet: a gyújtótrafó 2-pólusú csatlakozójának **BARNA/FEHÉR** szála (trafó negatív, a gyújtásmodul kapcsolt kimenete) 1 kΩ 1 W ellenálláson át |
| **Gyújtáselvétel (Spark Cut)** | `GPIO 19` | PC817 + 2N2222 NPN tranzisztor. Kollektor: a gyújtásmodul (igniter) szürke 3-pólusú csatlakozójának **BARNA/SÁRGA** szála (ECU → igniter vezérlőjel, "IB"), emitter: GND |
| **Állapotjelző LED** | `GPIO 2` | Kék beépített LED (tiltáskor világít / OTA frissítés alatt folyamatos) |
| **Fizikai Rajt Kapcsoló** | `GPIO 23` | Kézifékkar / kuplungpedál kapcsoló $\to$ GND (belső felhúzó ellenállás aktív) |

> ⛔ **A tiltó tranzisztort SOHA ne kösd a trafó negatív (-) pólusára (barna/fehér szál)!** Ott a gyújtáskor több száz voltos tüskék vannak: a 2N2222 (40 V) tönkremegy, jellemzően zárlatosra, ami menet közben leállítja a motort. A tiltás mindig az ECU vezérlőszálára (barna/sárga) megy.

> 📖 Részletes kapcsolási rajz és méretezési útmutató a [WIRING_AND_SETUP_GUIDE.md](WIRING_AND_SETUP_GUIDE.md) fájlban található!

---

## 💻 Fordítás és Feltöltés (PlatformIO)

```powershell
# Fordítás (a firmware.bin és a version.json a repo gyökerébe is bekerül)
pio run

# Feltöltés USB-n (ha szükséges)
pio run -t upload

# Feltöltés Wi-Fi-n (laptop az autó Wi-Fi-jén)
pio run -e esp32dev_wifi -t upload
```

### 🚀 Új verzió kiadása (egy parancs + push)
1. **Emeld a verziót** a `swift_show_tuning/version.h`-ban: `FW_VERSION` (pl. `"2.1.0"`) és `FW_VERSION_CODE` (pl. `20100` = fő·10000 + al·100 + javítás). A vezérlők csak akkor ajánlják fel a frissítést, ha a `FW_VERSION_CODE` nagyobb a futónál.
2. **`pio run`** – a `scripts/post_build.py` a repo gyökerébe másolja a `firmware.bin`-t, és megírja mellé a `version.json`-t (verzió, kód, méret, MD5, build idő). Figyelmeztet, ha elfelejtetted emelni a verziót.
3. **Commit + push:** `firmware.bin` + `version.json` (és a forráskód) a `main` ágra.
4. Kész: a GitHub pár percen belül (a raw.githubusercontent.com gyorsítótára ~5 perc) kiszolgálja az új fájlokat, onnantól a dashboard és a vezérlő is látja az új verziót.

> ⚠️ A platform rögzítve van (`platformio/espressif32 @ 7.0.1` = Arduino-ESP32 2.0.17), hogy egy PlatformIO frissítés ne váltson csendben 3.x magra. A partíciós táblát (`default.csv`) ne módosítsd: a már beszerelt vezérlőkön OTA-val nem változtatható.

---

## ⚖️ Figyelmeztetés / Diszkrét Használat
A rendszer szikraelvétellel működik, melynek során elégetlen üzemanyag jut a kipufogórendszerbe.  
Kizárólag zárt pályás rendezvényekre, kiállításokra és show használatra készült! Katalizátorral felszerelt rendszer esetén a katalizátor károsodhat. Használata felelősséggel javasolt!
