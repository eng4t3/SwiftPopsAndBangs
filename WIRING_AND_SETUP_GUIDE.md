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
 Fordulatszám Jel (Barna/Fehér szál, trafó 2-pólusú csatlakozó = trafó negatív) ---+ | |
                                                                                   | | |
 Gyújtásmodul "IB" Vezérlőszál (Barna/Sárga, igniter szürke csatlakozó) ----------+| | |
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
Trafó 2-pólusú csatl.                     +--------\_/--------+
BARNA/FEHÉR szál   --[ 1kΩ 1W ]----+----> | 1 (Anód)     4 (C)| ----> ESP32 GPIO 18
(trafó negatív)                    |      |                   |
                              [ 1N4148 ]  |                   |
                              (Csík a     |                   |
                               Pin 1-re)  |                   |
                                   |      |                   |
Autó Test (GND) -------------------+----> | 2 (Katód)    3 (E)| ----> ESP32 GND
                                          +-------------------+
```
* **Működés**: A barna/fehér szál a trafó negatív pólusa (a gyújtásmodul kapcsolt kimenete). Amikor a gyújtásmodul nem tölti a trafót, ezen ~12 V van (az opto LED világít, a GPIO 18 alacsony). Töltés közben ~1 V (a LED nem világít). A szikra pillanatában a feszültség visszaugrik 12 V-ra, egy rövid, több száz voltos tüskével. A GPIO 18 **lefutó éle = egy gyújtás**, fordulatonként 2 db.
* Az `1 kΩ (1W)` ellenállás a folyamatos áramot ~11 mA-re korlátozza. A szikra tüskéjénél rövid ideig ennél jóval nagyobb áram folyik.
* A `1N4148` dióda **fordítva, párhuzamosan** kerül a LED-del (a csíkos vége a Pin 1-re, a másik vége a Pin 2-re), és a negatív tüskéket vezeti el. **Ne kösd sorba!** Sorba kötve, rossz irányban, lezárja a LED-et, és az RPM mindig 0 marad.
* A PC817 fototranzisztora a belső pull-up-pal ellátott **GPIO 18**-at rántja le 0 V-ra, amíg a LED világít.
* **Fontos:** tiltás alatt a gyújtásmodul nem tölti a trafót, ezért **ilyenkor nincs fordulatszám-impulzus**. A firmware ezt kezeli: a kimaradt gyújtásokat időzítéssel becsüli, és csak a valódi szikrákból mér.
* **Leválasztás:** az opto miatt a trafó tüskéi nem jutnak az ESP32 bemenetére, de a föld közös (az ESP32 a tápegységen keresztül az autó testén van).
* **Opcionális javítás:** az 1 kΩ helyett 3,3–4,7 kΩ (1 W) kisebb tüskeáramot enged a LED-re, és a jel így is bőven elég. Egy külső 4,7 kΩ-os felhúzó ellenállás a GPIO 18 és a 3,3 V között élesebb jelélt ad.

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

> ✅ **Bevált bekötés** (ebben az autóban így működik): RPM = trafó csatlakozó **barna/fehér**, tiltás = igniter csatlakozó **barna/sárga**, +12 V = **fekete/fehér**, GND = hengerfej / szívósor testcsavar.

### 1. Fordulatszám Jel (Barna/Fehér vezeték)
* **Hol találod?**
  * A tűzfalon lévő hengeres gyújtótrafó **2-pólusú csatlakozójában** a **BARNA/FEHÉR** szál (főleg barna, vékony fehér csíkkal). Ez a trafó negatív pólusa: a gyújtásmodulból jön, és ugyanez a jel megy a gyári fordulatszámmérőre is.
  * A másik szál ugyanitt a fekete/fehér +12 V.
  * *Alternatíva*: a tűzfalon lévő sárga gumisapkás 6-pólusú diagnosztikai csatlakozó barna szála, vagy a műszeregység fordulatszámmérőjének barna szála.
  * Erre megy az `1 kΩ 1W` ellenállás és az Opto 1 (lásd a **B.** szekciót). **Erre a szálra SOHA ne kösd a tiltó tranzisztort!**

### 2. Gyújtás Tiltó Szál (IB szál, Barna/Sárga vezeték)
* **Hol találod?**
  * A trafótól jobbra lévő **lapos kis gyújtásmodul (igniter) szürke, 3-pólusú csatlakozójában**:
    * **fekete/fehér** = +12 V táp,
    * **barna/fehér** = a trafó negatívjára megy (ez az RPM jel, lásd fent),
    * **barna/sárga** = **az ECU-ból jövő vezérlőszál (IB)**. **Erre kell rácsatlakozni.**
  * Ebbe a barna/sárga vezetékbe kell belekötni (T-leágazással) a 2N2222 kollektorát.

### 3. Gyújtáskapcsolt +12V Táp (Fekete/Fehér vezeték)
* **Hol találod?**
  * A gyújtótrafó pozitív (+) oldalára menő **fekete alapon fehér csíkos (`BLK/WHT`)** tápkábel (csak ráadott gyújtásnál, kulcs II-es állásban van rajta 12 V, így álló autóban nem meríti az akksit).
  * *Alternatíva*: A műszerfal alatti biztosítéktáblánál a **7-es számú biztosíték (Meter/Ignition)** mögött ugyanez a szál.
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
   * A fejlécben megjelenik a „KAPCSOLÓDVA” állapot és a firmware verziója.
   * **Padteszt:** a Biztonság kártyán legyen bekapcsolva a **Rendszer élesítve** főkapcsoló. Válts **SHOW MÓD**-ra, és tartsd nyomva a nagy gombot. Álló motornál (legalább 2 mp gyújtásjel nélkül) az ESP32 kék LED-je világít, és a panelen a tiltás kimenet aktív. Amint jön egy gyújtásjel, a padteszt azonnal leáll. A GPIO 23 kapcsoló padtesztet nem indít.
5. **Autóba szerelés**:
   * Kösd be a 4 szálat a Swiftbe.
   * Indítsd el a motort alapjáraton: a telefonos kijelzőn azonnal látnod kell a gyári $\sim 850\text{ RPM}$ alapjáratot.
   * Élvezd a show-t, a lángokat és a durrogást!
