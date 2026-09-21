# 🏁 Suzuki Swift 1.3 8V (G13BA) - ESP32 Show Tuning & Pops and Bangs

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange.svg)](https://platformio.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

ESP32 alapú, Wi-Fi Web Dashboarddal vezérelhető motorsport show-tuning és gyújtáselvételes fordulatszám-szabályzó rendszer Suzuki Swift 1.3 8V (G13BA motorkód, osztófejes gyújtás) típushoz.

---

## 📱 Közvetlen Letöltés Telefonra (Web OTA Frissítéshez)

Ha a vezérlő már be van szerelve a kocsiba, nem kell kiszerelned! Csak töltsd le a legújabb lefordított firmware-t közvetlenül a telefonodra:

👉 **[📥 firmware.bin LETÖLTÉSE (Közvetlen link)](https://raw.githubusercontent.com/eng4t3/SwiftPopsAndBangs/main/firmware.bin)**  
*(Vagy a GitHub felületen a `firmware.bin` fájlra kattintva a **Download raw file** gombbal.)*

### 📲 Frissítés menete telefonról (3 lépés):
1. **Töltsd le** a fenti `firmware.bin` fájlt a telefonod *Letöltések* mappájába.
2. **Csatlakozz** az autó Wi-Fi hálózatára:
   - **SSID:** `Swift-PopsAndBangs`
   - **Jelszó:** `swift123`
   - Nyisd meg a böngészőt: `http://192.168.4.1`
3. **Telepítsd vezeték nélkül:**
   - Görgess le a **„📡 Vezeték nélküli frissítés (OTA)”** szekcióhoz.
   - Koppints a **`📁 FIRMWARE (.BIN) KIVÁLASZTÁSA`** gombra, és válaszd ki a letöltött fájlt.
   - Nyomd meg a zöld **`🚀 TELEPÍTÉS VEZETÉK NÉLKÜL`** gombot (~15 másodperc, automatikus újraindulás).

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
| **Fordulatszám bemenet (Tach In)** | `GPIO 18` | PC817 optocsatoló kimenet (Gyújtótrafó NEGATÍV (-) pólusáról 1kΩ ellenállással) |
| **Gyújtáselvétel (Spark Cut)** | `GPIO 19` | NPN tranzisztor (2N2222 / BD139) bázisa 1kΩ ellenálláson át (Kollektor a trafó (-) pólusra, emitter GND-re) |
| **Állapotjelző LED** | `GPIO 2` | Kék beépített LED (tiltáskor világít / OTA frissítés alatt folyamatos) |
| **Fizikai Rajt Kapcsoló** | `GPIO 23` | Kézifékkar / kuplungpedál kapcsoló $\to$ GND (belső felhúzó ellenállás aktív) |

> 📖 Részletes kapcsolási rajz és méretezési útmutató a [WIRING_AND_SETUP_GUIDE.md](WIRING_AND_SETUP_GUIDE.md) fájlban található!

---

## 💻 Fordítás és Feltöltés (PlatformIO)

```powershell
# Fordítás
platformio run

# Feltöltés USB-n (ha szükséges)
platformio run -t upload
```

---

## ⚖️ Figyelmeztetés / Diszkrét Használat
A rendszer szikraelvétellel működik, melynek során elégetlen üzemanyag jut a kipufogórendszerbe.  
Kizárólag zárt pályás rendezvényekre, kiállításokra és show használatra készült! Katalizátorral felszerelt rendszer esetén a katalizátor károsodhat. Használata felelősséggel javasolt!
