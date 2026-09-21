# Suzuki Swift 1.3 8V (2000 - G13BA) Show Tuning - Mester Bekötési és Kapcsolási Kézikönyv

Ez a dokumentum a **2000-es évjáratú Suzuki Swift 1.3 8V (G13BA motorkódos)** típushoz készült teljes, alkatrészszintű bekötési és összeszerelési útmutató.

---

## 1. Rendszer Áttekintő Blokkvázlat

```
====================================================================================================
                        SUZUKI SWIFT 1.3 8V (G13BA) MOTORTÉR & MŰSZERFAL
====================================================================================================

 Gyújtáskapcsolt +12V (Fekete/Fehér) ----> [ Lengő Biztosítékház 1A ] ------------------+
                                                                                       |
 Hengerfej / Karosszéria Testcsavar -------------------------------------------------+ |
                                                                                     | |
 Fordulatszám Jel (Tachometer, Barna szál a sárga diagnosztikai csatlakozóból) ----+ | |
                                                                                   | | |
 Gyújtásmodul "IB" Vezérlőszál (ECU és trafó közötti vékony jelvezeték) ----------+| | |
                                                                                  ||| |
                                                                                  vvv v
+--------------------------------------------------------------------------------------------------+
|                            ESP32 SHOW TUNING VEZÉRLŐ PANEL (NYÁK)                                |
|                                                                                                  |
|   [ SORKAPOCS 1: TÁPELLÁTÁS ]                 [ SORKAPOCS 2: MOTOR JELVEZETÉKEK ]                |
|   Pin 1: +12V IN (Biztosítéktól)              Pin 1: RPM IN (Barna fordulatszám szál)            |
|   Pin 2: GND IN (Hengerfej test)              Pin 2: SPARK CUT OUT (IB gyújtás tiltó szál)       |
|            |                                            |                  |                     |
|            v                                            v                  v                     |
|     +--------------+                             +------------+     +--------------+             |
|     | 1N4007 Dióda |                             | PC817 OPTO |     |  2N2222 NPN  |             |
|     +--------------+                             | (Leválaszt)|     | (Lehúzó BJT) |             |
|            |                                     +------------+     +--------------+             |
|            v                                            |                  ^                     |
|   +------------------+                                  |                  |                     |
|   | LM2596 BUCK (5V) |                                  v                  |                     |
|   +------------------+                           +----------------------------------+            |
|     |              |                             |        ESP32 MIKROKONTROLLER     |            |
|   +5V OUT         GND                            |                                  |            |
|     |              |                             |  GPIO 18 <--- RPM Pulzus (Bemenet|            |
|   [470µF]       [100nF]                          |  GPIO 19 ---> Spark Cut (Kimenet)|            |
|     |              |                             |  Wi-Fi AP: "Swift-PopsAndBangs"   |            |
|     +-------+------+                             +----------------------------------+            |
|             |                                                                                    |
+-------------|------------------------------------------------------------------------------------+
              |
              v (Wi-Fi WebSockets - 30 FPS Élő Telemetria)
     +-------------------+
     | OKOSTELEFON / UI  |  http://192.168.4.1 (Műszeregység, 2-Step gomb, Tiltás állítás)
     +-------------------+
```

---

## 2. Részletes Alkatrészszintű Kapcsolási Rajz

### A. Tápellátás Szekció (12V $\rightarrow$ Stabil 5.00V)
```
                                                                                 ESP32 VIN (5V)
                                                                                       ^
                                                                                       |
Autó +12V (IGN) ---> [ 1A Bizti ] ---> [ 1N4007 ] ---> (+) IN  [ LM2596 ] (+) OUT ----+----+----+
                                      (Csík jobbra)             [ BUCK   ]                  |    |
                                                       (-) IN  [ MODUL  ] (-) OUT ----+    |    |
                                                                                      |  [+]   [ ]
Autó Test (GND) ----------------------------------------------------------------------+  470µF 100nF
                                                                                      |  25V   50V
                                                                                      |   |     |
                                                                                      +---+-----+
                                                                                      |
                                                                                      v
                                                                                  ESP32 GND
```
* **1N4007 Dióda**: A fehér csík az LM2596 IN+ felé néz. Véd a fordított bekötéstől.
* **Kondenzátorok**: 
  * A `470 µF` elektrolit kondi **negatív csíkja** a GND-re megy, a pozitív lába az 5V-ra.
  * A `100 nF` kerámia kondi közvetlenül az ESP32 5V és GND lába mellé kerül zajszűrésnek.

---

### B. Fordulatszám Bemeneti Szekció (Tachometer $\rightarrow$ GPIO 18)
```
                                        DIL-08 FOGLALAT (PC817 - Opto 1)
Autó Barna Kábel                          +--------\_/--------+
(Fordulatszám Jel) --[ 1kΩ 1W ]---------> | 1 (Anód)     4 (C)| ----> ESP32 GPIO 18
                                          |                   |
Autó Test (GND) <----+---[ 1N4148 ]------ | 2 (Katód)    3 (E)| ----> ESP32 GND
                     |   (Csík Pin 1-re)  +-------------------+
                     |
(Közös motor test) --+
```
* **Működés**: Amikor a motor forog, a barna vezetéken 12V-os impulzusok érkeznek (fordulatonként 2 db).
* Az `1 kΩ (1W)` ellenállás lecsökkenti az áramot kíméletes $\sim 12\text{ mA}$-re.
* A `1N4148` dióda védi az optocsatoló LED-jét a negatív feszültségtüskéktől (a dióda fekete gyűrűs vége a Pin 1 felé néz).
* A PC817 fototranzisztora a belső pull-up-pal ellátott **GPIO 18**-at rántja le 0V-ra minden gyújtási eseménynél.
* **100% galvanikus leválasztás**: az autó elektromos tüskéi soha nem érhetik el az ESP32-t!

---

### C. Failsafe Gyújtás Tiltó Szekció (GPIO 19 $\rightarrow$ Igniter IB szál)
```
                                        DIL-08 FOGLALAT (PC817 - Opto 2)
ESP32 GPIO 19 -------[ 330Ω ]-----------> | 1 (Anód)     4 (C)| ----+
                                          |                   |     |
ESP32 GND ------------------------------> | 2 (Katód)    3 (E)| --+ |
                                          +-------------------+   | |
                                                                  | |
                                +---------------------------------+ |
                                |                                   |
                                v                                   |
                           [ 1kΩ 1/4W ]                             |
                                |                                   |
                                v                                   |
                            Bázis (B)                               |
                       2N2222A NPN Tranzisztor                      |
                           Kollektor (C) ---------------------------+----> Gyújtásmodul "IB" szála
                           Emitter   (E) --------------------------------> Motor / Hengerfej Test (GND)
```
* **Működés**:
  * **Normál üzemben / Kikapcsolt ESP32 esetén**: A 2N2222 bázisa 0V-on van, a tranzisztor lezárt (szakadás). Az ECU gyújtásjele akadálytalanul átjut a gyújtásmodulba $\rightarrow$ **az autó gyári módon indul és jár, sosem tud leállni!**
  * **Tiltáskor (Pops & Bangs / 2-Step / Leszabályzás)**: Az ESP32 GPIO 19 HIGH szintre vált. A PC817 kinyit, kinyitja a 2N2222-t, ami a földre rántja a gyújtásmodul IB bemenetét. A trafó primer köre nem töltődik fel, így a gyújtás kimarad, míg a benzin beáramlik a kipufogóba és berobban!

---

## 3. Alkatrész Lábkiosztási Útmutató (Pinout)

### PC817 Optocsatoló (DIL-08 foglalatba dugva)
```
           Felülnézet (Pont a bal felső sarokban)
                 +-------\_/-------+
        Pin 1 -> | [o]           4 | <- Pin 4 (Kollektor)
        Pin 2 -> |               3 | <- Pin 3 (Emitter)
                 +-----------------+
```
*(Ha DIL-08-as foglalatot használsz, egyszerűen a foglalat bal felébe (1, 2, 7, 8 lábak helyére) dugd be az optót).*

* **Screw Terminal Blocks (5.08mm pitch)**:
  * 2x 2-pin block (`TBG-5-PB-2P-GN`, Cikkszám: 100.245.29)
    - Sorkapocs 1: +12V és Test (GND)
    - Sorkapocs 2: Tachometer (RPM) és Gyújtás Tiltás (IB)

---

### 2N2222A / 2N2222 NPN Tranzisztor (TO-92 Tokozás)
```
             Lapos fele néz feléd:
                 +-----------+
                 |  2N2222A  |
                 +-----------+
                   |   |   |
                   1   2   3
                   E   B   C

    Láb 1 (balra):   Emitter (E)   --> Hengerfej Test (GND)
    Láb 2 (középen): Bázis (B)     --> 1 kΩ ellenállás az optó Pin 3-ról
    Láb 3 (jobbra):  Kollektor (C) --> Gyújtásmodul IB szála + Optó Pin 4
```

---

### Diódák Polaritása (Csík jelölés)
```
        1N4007 és 1N4148:
           Anód (+)                     Katód (-)
           ========[       |   ]========
                           ^
                     EZ A CSÍK (Gyűrű a diódán)
```

---

## 4. Próbanyák (HS-005) Elrendezési Terv

A `83 x 54 mm`-es próbapanelen így helyezd el az alkatrészeket a legtömörebb és legáttekinthetőbb vezetékezéshez:

```
+-----------------------------------------------------------------------+
|  [ SORKAPOCS 1 ]                      [ SORKAPOCS 2 ]                 |
|  [ 12V ] [ GND ]                      [ TACH ] [ TILTÁS ]             |
|                                                                       |
|  [1N4007]   +--------------------+     [1kΩ 1W]   [330Ω]              |
|             |  LM2596 DC-DC      |                                    |
|  [470µF]    |  Step-Down Modul   |     [DIL-08 #1] [DIL-08 #2]        |
|  [100nF]    |  (Trimmer 5.00V!)  |      (Tach Opto) (Cut Opto)        |
|             +--------------------+                                    |
|                                        [1N4148]   [1kΩ]  [2N2222A]    |
|                                                                       |
|  ==================== HÜVELYSOR BAL (15/19 pin) ====================  |
|                                                                       |
|                     [ ESP32 DEVKIT PANEL HELYE ]                      |
|                                                                       |
|  ==================== HÜVELYSOR JOBB (15/19 pin) ===================  |
+-----------------------------------------------------------------------+
```

---

## 5. Suzuki Swift 1.3 8V (2000 - G13BA) Autós Csatlakozási Pontok

### 1. Fordulatszám Jel (Barna vezeték)
* **Hol találod?** 
  * A motortérben, a tűzfalon az ablaktörlő motor mellett van egy sárga gumisapkás **6-pólusú diagnosztikai csatlakozó**.
  * Ebben a csatlakozóban van egy **tömör BARNA (`BRN`) szál**. Erre kell rácsatlakozni (ez közvetlenül a gyári tachométer jele).
  * *Alternatíva*: A műszeregység mögött a fordulatszámmérő óra csatlakozójának barna szála.

### 2. Gyújtás Tiltó Szál (IB szál)
* **Hol találod?**
  * A hengerfej jobb (váltó felőli) oldalán, a gyújtáselosztó mellett található a gyújtástrafó és a hozzá csavarozott kis fekete gyújtásmodul (igniter).
  * A gyújtásmodulba menő 2 vagy 3 szálas csatlakozóban keresd az **ECU-ból érkező vékony vezérlőszálat** (gyárilag általában **barna/fehér** vagy **fekete/fehér**).
  * Ebbe a vezetékbe kell belekötni (T-leágazással) a 2N2222 Kollektorát.

### 3. Gyújtáskapcsolt +12V Táp (Fekete/Fehér vezeték)
* **Hol találod?**
  * A műszerfal alatti biztosítéktáblánál a **7-es számú biztosíték (Meter/Ignition)** mögött, vagy a gyújtáskapcsoló vastagabb kábelkötegében a **fekete alapon fehér csíkos (`BLK/WHT`)** szál.
  * **KÖTELEZŐ**: Közvetlenül a leágazás után építsd be a **lengő biztosítékházat az 1A-es biztosítékkal**!

### 4. Testelés (GND)
* **Hol találod?**
  * Egy legalább 0.75 mm²-es fekete vezetéket szerelj fel M6-os szemes saruval, és csavard rá a **hengerfej vagy a szívósor egyik gyári testcsavarjára** (a festetlen fém felületre!).

---

## 6. Első Élesztési & Tesztelési Ellenőrzőlista (Lépésről lépésre)

1. **Összerakás ellenőrzése**:
   * Forraszd össze az alkatrészeket a fenti rajz szerint.
   * **AZ ESP32-T MÉG NE DUGD BE A HÜVELYSORBA!**
2. **Feszültség beállítás (Multiméterrel)**:
   * Adj 12V-ot a Sorkapocs 1-re (akár egy 12V-os dugasztápról vagy az autó akksijáról).
   * Mérj rá multiméterrel a hüvelysor azon pontjára, ahova az ESP32 5V (VIN) és GND lába esik.
   * Tekerd az LM2596 kék trimmerpotiját csavarhúzóval az óramutatóval ellentétes irányba, amíg a multiméter **PONTOSAN 5.00V-ot** mutat!
3. **ESP32 behelyezése**:
   * Áramtalanítsd a panelt.
   * Dugd be a felprogramozott ESP32-t és a két PC817 optocsatolót a foglalatokba.
   * Kapcsold vissza a tápot: az ESP32 kék LED-je felvillan.
4. **Wi-Fi & Gomb Teszt**:
   * Csatlakozz a telefonoddal a `Swift-PopsAndBangs` Wi-Fi-re (jelszó: `swift123`).
   * Nyisd meg a böngészőben: `http://192.168.4.1`.
   * A mutatón lefut a nyitó söprés (0 $\rightarrow$ 8000 $\rightarrow$ 0 RPM).
   * Kapcsold be a **Master Arm**-ot, és tartsd nyomva a piros **🔥 2-STEP / FLAMES 🔥** gombot: az ESP32 kék LED-je és a panelen a tiltás kimenet aktívvá válik.
5. **Autóba szerelés**:
   * Kösd be a 4 szálat a Swiftbe.
   * Indítsd el a motort alapjáraton: a telefonos kijelzőn azonnal látnod kell a gyári $\sim 850\text{ RPM}$ alapjáratot.
   * Élvezd a show-t, a lángokat és a durrogást!
