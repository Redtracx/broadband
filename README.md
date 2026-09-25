# broadband

Ein einfacher **Speedtest und Bandbreitentest** mit Qt 6 GUI, geschrieben in C++17. Läuft unter Linux, Windows und macOS.

- **Speedtest:** Ping, Jitter, Download und Upload, gemessen gegen [speed.cloudflare.com](https://speed.cloudflare.com), mit Live-Diagramm. Dazu der **Ping unter Last** (Bufferbloat), das Cloudflare-Rechenzentrum und die verwendete IP-Version
- **Bandbreite unter Last:** Kommt die vertraglich zugesagte Bandbreite auch dann an, wenn mehrere Geräte gleichzeitig laden? Das Programm simuliert stufenweise 1, 2, 4, 8, 16 … gleichzeitige Clients und zeigt pro Stufe Gesamtdurchsatz, Anteil pro Client, Ping unter Last und Prozent vom Vertrag.
- **LAN-Test** zwischen zwei eigenen Rechnern. Der passende Testserver ist eingebaut, mit GUI oder headless.
- **Messreihen:** Tests mehrmals nacheinander ausführen, mit Pause dazwischen. Das Programm erkennt Ausreißer und Trends und sagt, ob die Werte belastbar sind oder nur ein Zufallstreffer waren.
- **Verlauf:** Alle Messungen bleiben zwischen den Starts erhalten.
- **Kommandozeile:** `broadband --run` misst ohne GUI und gibt Text, JSON oder CSV aus, z. B. für cron. Die Ergebnisse landen im selben Verlauf wie die der GUI.
- **Vertrag prüfen:** Vorprüfung nach den Kriterien der Bundesnetzagentur für eine Minderung: Lohnt sich die offizielle Breitbandmessung?
- **Robust:** Bricht eine einzelne Verbindung ab, wird sie neu aufgebaut, statt die ganze Messung zu verwerfen.
- **Excel-Export** aller Messungen als gestaltete Arbeitsmappe (.xlsx) mit Übersicht, Auswertung und Diagrammen
- Keine Abhängigkeiten außer Qt (Widgets + Network)

![Speedtest](docs/screenshot.png)

## Inhalt

- [Funktionsweise](#funktionsweise)
- [Bandbreite unter Last](#bandbreite-unter-last)
- [Messreihen und Excel-Export](#messreihen-und-excel-export)
- [Vertrag prüfen](#vertrag-prüfen)
- [Kommandozeile und Automatisierung](#kommandozeile-und-automatisierung)
- [Windows](#windows)
- [Bauen](#bauen)
- [Benutzung](#benutzung)
- [Tests](#tests)
- [Projektstruktur](#projektstruktur)
- [Hinweise zur Messgenauigkeit](#hinweise-zur-messgenauigkeit)
- [Sicherheit und Datenschutz](#sicherheit-und-datenschutz)

## Funktionsweise

Der Test läuft in drei Phasen:

| Phase    | Was passiert                                                                                   | Ergebnis              |
|----------|------------------------------------------------------------------------------------------------|-----------------------|
| Latenz   | 1 + 10 kleine HTTP-Requests nacheinander. Der erste baut die Verbindung auf und zählt nicht.   | Median-Ping, Jitter   |
| Download | *n* parallele Verbindungen laden für die eingestellte Dauer fortlaufend Daten (je 25 MB). Nebenher läuft alle 250 ms ein Ping. | Mbit/s, Ping unter Last |
| Upload   | *n* parallele Verbindungen senden fortlaufend zufällige Daten (je 8 MB), ebenfalls mit Ping.   | Mbit/s, Ping unter Last |

- **Jitter** ist die mittlere Abweichung zwischen aufeinanderfolgenden Ping-Messungen (wie in RFC 3550).
- Die ersten **20 %** jeder Transferphase gelten als Aufwärmzeit (TCP Slow Start) und fließen nicht ins Ergebnis ein.
- Das Live-Diagramm zeigt einen gleitenden Mittelwert über 1 Sekunde.
- Es wird bewusst **HTTP/1.1** verwendet, damit jeder Stream eine eigene TCP-Verbindung bekommt. Bei HTTP/2 würden alle Streams über eine einzige Verbindung laufen.
- Die Upload-Daten sind zufällig, damit komprimierende Proxys das Ergebnis nicht verfälschen.
- **Ping unter Last:** Steigt der Ping während Download oder Upload stark an (mehr als 100 ms über dem Leerlauf), puffert der Router zu viel (*Bufferbloat*). Videocalls und Spiele ruckeln dann, sobald jemand lädt. Abhilfe schafft oft SQM bzw. „Smart Queue“ im Router.
- **Streams:** Jeder Stream hat eine eigene Verbindung und einen eigenen `QNetworkAccessManager`. Deshalb gilt Qts Grenze von 6 Verbindungen pro Host nicht, und es sind bis zu 32 Streams möglich.
- **Abgebrochene Verbindungen** werden nach 250 ms neu aufgebaut, und die Messung läuft weiter. Wie viele Übertragungen neu gestartet wurden, steht im Ergebnis und im Excel-Bericht. Die Messung bricht erst ab, wenn es in einer Phase mehr als `max(3, 2 × Streams)` Fehler gibt oder wenn der Server die Anfrage mit einem 4xx-Status ablehnt.
- **Server und IP-Version:** Cloudflare meldet das Rechenzentrum (z. B. `FRA`) und die öffentliche IP, unter der du ankommst. Daraus zeigt das Programm auch, ob die Verbindung über IPv4 oder IPv6 läuft. Neben dem Server-Feld lässt sich *nur IPv4* oder *nur IPv6* erzwingen, denn beide Wege können unterschiedlich schnell sein. Dafür wird der Hostname vorab aufgelöst. Die Anfragen gehen dann an eine Adresse der gewählten Familie, Host-Header und TLS-Zertifikatsprüfung verwenden weiterhin den Namen. Hinter einem Proxy geht das nicht, das Programm meldet das dann.

### Protokoll

Client und eingebauter Server sprechen dasselbe einfache HTTP-Protokoll wie Cloudflare:

```
GET  /__down?bytes=N    → Antwort mit N Bytes
POST /__up              → Request-Body wird gelesen und verworfen
```

Deshalb funktioniert jeder kompatible Server als Ziel, sowohl Cloudflare als auch `broadband --server`.

## Bandbreite unter Last

Der Speedtest zeigt, wie schnell die Leitung **maximal** ist. Im Alltag laden aber oft mehrere Geräte gleichzeitig, und genau dann wird es häufig deutlich schlechter. Der Tab **Bandbreite unter Last** prüft diesen Fall:

![Bandbreite unter Last](docs/loadtest.png)

1. Die vertragliche Bandbreite eintragen, z. B. 250 Mbit/s. Das ist optional.
2. Die Stufen wählen, standardmäßig `1, 2, 4, 8, 16` gleichzeitige Clients, und die Dauer je Stufe festlegen.
3. **Test starten**

Zuerst misst das Programm den Ping im Leerlauf. Danach läuft pro Stufe die angegebene Zahl Clients gleichzeitig. Jeder Client hat eine **eigene Verbindung**, verhält sich also wie ein eigenes Gerät. Die Tabelle zeigt pro Stufe:

| Spalte              | Bedeutung                                                                       |
|---------------------|---------------------------------------------------------------------------------|
| Gesamt              | Summe aller Clients: Wie viel liefert die Leitung insgesamt?                    |
| vom Vertrag         | Gesamt im Verhältnis zur vertraglichen Bandbreite (grün ab 90 %)                |
| Ø / Min – Max       | Wie viel bekommt jeder einzelne Client, und wie gerecht wird geteilt?           |
| Ping unter Last     | Reaktionszeit, während die Leitung voll ist (Stichwort *Bufferbloat*)           |
| Datenmenge          | Übertragene Daten in dieser Stufe                                               |

Darunter steht eine Auswertung in Klartext: ob der Vertrag erreicht wird, ab wie vielen Clients die Leitung voll ist, ob der Gesamtdurchsatz bei vielen Clients einbricht, wie stark der Ping steigt und wie viele GB pro Stunde das entspricht.

**Tipps:**
- Für einen **Dauertest**, etwa gegen Drosselung oder abendliche Engpässe, eine einzelne Stufe mit langer Dauer wählen: Stufen `8`, Dauer `300 s`. Einbrüche sind dann im Verlauf gut zu sehen.
- Die Schwelle von 90 % entspricht der Vorgabe der Bundesnetzagentur für die vertragliche Maximalgeschwindigkeit. Rechtlich verwertbar, z. B. für eine Minderung, ist aber nur eine Messung mit der offiziellen [Breitbandmessung](https://breitbandmessung.de).
- Das Programm simuliert mehrere Clients **von einem Rechner aus**. Das testet den Anschluss. Echte Geräte im WLAN teilen sich zusätzlich die Funkzelle. Wenn das WLAN der Engpass ist, stellt man den Rechner zum Vergleich einmal per Kabel direkt an den Router.

## Messreihen und Excel-Export

Eine einzelne Messung kann täuschen: Ein Update im Hintergrund, ein kurzer Engpass beim Provider, und schon ist der Wert nur halb so hoch. Deshalb lassen sich beide Tests **mehrmals nacheinander** ausführen:

- **Speedtest:** *Wiederholungen* und *Pause* einstellen, z. B. `5 ×` mit `30 s` Pause. Für eine Beobachtung über den Abend eignen sich z. B. `20 ×` mit `600 s`.
- **Bandbreite unter Last:** *Durchläufe* und *Pause dazwischen* einstellen. Das Diagramm zeigt alle Durchläufe hintereinander. Ausgewertet wird der Median je Stufe.

Nach jeder Messung wird die Serie neu ausgewertet:

| Kennzahl    | Bedeutung                                                                                                   |
|-------------|-------------------------------------------------------------------------------------------------------------|
| Median      | Der mittlere Wert der Serie. Einzelne Ausreißer beeinflussen ihn kaum, deshalb ist er die Hauptkennzahl.     |
| Schwankung  | Standardabweichung im Verhältnis zum Mittelwert: unter 5 % sehr stabil, unter 15 % stabil, darüber schwankend |
| Ausreißer   | Messungen, die weit vom Rest abweichen (mehr als 3 robuste Standardabweichungen und mehr als 10 %). Ab 3 Messungen. |
| Trend       | Steigen oder fallen die Werte über die Serie hinweg um mindestens 10 %? Ab 4 Messungen.                     |

Ausreißer werden im Verlauf rot markiert. Die Schwankung bewertet das Programm ohne die Ausreißer: Ein einzelner Fluke macht die Leitung nicht „instabil“, er wird aber ausdrücklich benannt. Ein fallender Trend deutet auf zunehmende Auslastung (abends) oder eine Drosselung hin.

**Excel-Export …** speichert alle Messungen des Verlaufs als `.xlsx`. Die Datei lässt sich mit Excel und LibreOffice Calc öffnen:

- **Übersicht:** Kennzahlen je Server (Mittelwert, Median, Min, Max, Standardabweichung, Schwankung) mit farbiger Bewertung, Ausreißern, Trend und der Auswertung in Klartext
- **Speedtest:** jede Messung mit Rechenzentrum, IP-Version, Abweichung vom Median, Ping unter Last, neu aufgebauten Verbindungen, markierten Ausreißern, Autofilter, Formeln für Mittelwert/Median/Min/Max und Diagrammen für Download/Upload und Ping/Jitter je Messung
- **Bandbreite unter Last:** jede Stufe jedes Durchlaufs und ein Diagramm mit einer Linie pro Durchlauf, sodass Abweichungen sofort auffallen

### Verlauf

Jede abgeschlossene Messung wird sofort gespeichert und ist beim nächsten Start wieder da, im Speedtest-Verlauf und als letzter Lasttest. Die GUI und die Kommandozeile schreiben in dieselbe Datei. Messungen eines cron-Jobs erscheinen deshalb auch in der GUI, im Excel-Export und in der Vertragsprüfung.

| System  | Datei                                                   |
|---------|---------------------------------------------------------|
| Linux   | `~/.local/share/broadband/broadband/history.jsonl`      |
| Windows | `%APPDATA%\broadband\broadband\history.jsonl`          |
| macOS   | `~/Library/Application Support/broadband/broadband/history.jsonl` |

Das Format ist JSON Lines: ein Objekt pro Zeile, dasselbe wie bei `broadband --run --format json`. Beschädigte Zeilen werden beim Einlesen übersprungen. Mit der Umgebungsvariablen `BROADBAND_HISTORY` lässt sich eine andere Datei verwenden.

Mit **Verlauf leeren** werden alle gespeicherten Speedtests nach einer Rückfrage gelöscht, z. B. vor einer Messreihe an einem anderen Ort. Die Lasttests bleiben erhalten.

## Vertrag prüfen

Nach § 57 Abs. 4 TKG darfst du das Entgelt mindern, wenn die Geschwindigkeit erheblich vom Vertrag abweicht. Wie das nachzuweisen ist, legt die [Bundesnetzagentur](https://www.bundesnetzagentur.de/DE/Vportal/TK/InternetTelefon/Internetgeschwindigkeit/start.html) fest. Der Tab **Vertrag prüfen** wendet diese Regeln auf die Speedtests aus dem Verlauf an:

1. Maximal-, Normal- und Mindestgeschwindigkeit für Download und Upload aus dem Produktinformationsblatt eintragen.
2. An drei Tagen je 10 Mal messen. Am einfachsten mit **Messblock starten (5 × alle 5 min)**, zweimal am Tag im Abstand von mindestens 3 Stunden. Alternativ mit cron, siehe unten.
3. Die Tabelle zeigt je Messtag die Werte und ob das Messprotokoll eingehalten wurde. Darunter steht die Auswertung.

**Messprotokoll:** 30 Messungen an 3 Kalendertagen, je 10 pro Tag, mindestens 5 Minuten zwischen zwei Messungen und mindestens 3 Stunden zwischen der 5. und 6. Messung. Zwischen den Messtagen muss mindestens ein Kalendertag liegen, und alle Messtage müssen innerhalb von 14 Tagen liegen.

**Erhebliche Abweichung**, jeweils für Download oder Upload, wenn

- 90 % der Maximalgeschwindigkeit nicht an mindestens zwei der drei Messtage mindestens einmal erreicht werden,
- die normalerweise zur Verfügung stehende Geschwindigkeit nicht in 90 % der Messungen erreicht wird oder
- die Mindestgeschwindigkeit an mindestens zwei der drei Messtage jeweils mindestens einmal unterschritten wird.

Ausgewertet werden nur Messungen gegen Internet-Server, keine LAN-Tests. Solange noch keine 30 Messungen vorliegen, ist das Ergebnis als *vorläufig* gekennzeichnet. **Rechtlich verwertbar ist nur die offizielle [Breitbandmessung](https://breitbandmessung.de).** Diese Vorprüfung zeigt, ob sich der Aufwand lohnt.

## Kommandozeile und Automatisierung

Mit `--run` (Speedtest) oder `--load` (Lasttest) misst broadband ohne GUI. Jedes Ergebnis wird im [Verlauf](#verlauf) gespeichert, außer mit `--no-save`.

```bash
broadband --run                           # Speedtest, Ausgabe als Text
broadband --run -n 8 -d 15 --ip 6         # 8 Streams, 15 s je Richtung, nur IPv6
broadband --run --format json             # eine Zeile JSON, wie im Verlauf
broadband --run --format csv --no-header  # eine CSV-Zeile, zum Anhängen an eine Datei
broadband --load 1,4,16 --contract 250    # Lasttest mit Auswertung gegen den Vertrag
broadband --load 8 --upload -d 60 -u http://192.168.1.20:8080   # Upload im LAN
```

```
Server:    https://speed.cloudflare.com
Verbindung: Rechenzentrum FRA, IPv6
Ping:      12,4 ms (Jitter 1,1 ms)
Download:  243 Mbit/s (Ping unter Last 38 ms)
Upload:    41,8 Mbit/s (Ping unter Last 95 ms)
✔ Ping unter Last: 38 ms beim Download, 95 ms beim Upload (ohne Last 12 ms). …
```

Der Exit-Code ist `0` bei Erfolg, `1` wenn die Messung fehlschlägt und `2` bei ungültigen Optionen. Fehlermeldungen gehen nach stderr, sodass stdout bei JSON und CSV sauber bleibt.

**Beispiel cron** (`crontab -e`): 10 Messungen am Tag, je 5 um 9 und um 18 Uhr im Abstand von 10 Minuten. Das passt zum Messprotokoll der Bundesnetzagentur, und die Ergebnisse erscheinen im Tab *Vertrag prüfen*:

```
0,10,20,30,40 9,18 * * *  /usr/local/bin/broadband --run --format csv --no-header >> ~/speedtest.csv 2>> ~/speedtest.log
```

Unter Windows geht dasselbe mit der Aufgabenplanung (`broadband.exe --run`). Bitte die [Hinweise zu Cloudflare](#hinweise-zur-messgenauigkeit) beachten und nicht im Minutentakt messen.

## Windows

### Fertiges Paket

`broadband-<version>-windows-x64.zip` entpacken und `broadband.exe` starten. Eine Installation ist nicht nötig. Das Paket enthält nur die benötigten Qt- und MinGW-Laufzeit-DLLs. HTTPS läuft über den TLS-Stack von Windows (Schannel), OpenSSL wird nicht mitgeliefert.

Die `.exe` ist nicht digital signiert. Deshalb zeigt Windows SmartScreen beim ersten Start eventuell „Der Computer wurde durch Windows geschützt“. Über *Weitere Informationen → Trotzdem ausführen* startet das Programm.

### Unter Windows selbst bauen

Qt über den [Qt Online Installer](https://www.qt.io/download-qt-installer) installieren, mit der Komponente *MinGW 13.1* (oder MSVC). Danach in der „Qt 6.x (MinGW)“-Eingabeaufforderung:

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
windeployqt --release build\broadband.exe
```

### Unter Linux für Windows cross-kompilieren

Benötigt den MinGW-w64-Cross-Compiler (`sudo apt install g++-mingw-w64-x86-64-posix`) und Qt für *Windows/MinGW* sowie Qt für *Linux* (Host-Tools wie `moc`), jeweils in **derselben Version**. Beides bekommst du z. B. mit dem Qt Online Installer oder mit [aqtinstall](https://github.com/miurahr/aqtinstall).

```bash
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
    -DCMAKE_PREFIX_PATH=~/Qt/6.8.3/mingw_64 \
    -DQT_HOST_PATH=~/Qt/6.8.3/gcc_64
cmake --build build-win -j

# ZIP mit exe und den benötigten DLLs erzeugen
scripts/package-windows.sh build-win ~/Qt/6.8.3/mingw_64 \
    /usr/lib/gcc/x86_64-w64-mingw32/13-posix /usr/x86_64-w64-mingw32/lib
```

`package-windows.sh` wertet die Import-Tabellen der exe aus und kopiert nur die DLLs, die tatsächlich geladen werden. Am Ende listet es auch die Windows-Systembibliotheken auf, die das Programm verwendet.

Die CI (`.github/workflows/build.yml`) baut und testet zusätzlich nativ unter Windows und stellt das Ergebnis als Artefakt bereit.

## Bauen

Voraussetzungen: **CMake ≥ 3.19**, ein **C++17-Compiler** und **Qt ≥ 6.2** (Module *Widgets* und *Network*).

### Abhängigkeiten installieren

| System          | Befehl                                                                   |
|-----------------|--------------------------------------------------------------------------|
| Ubuntu / Debian | `sudo apt install build-essential cmake qt6-base-dev libgl1-mesa-dev`    |
| Fedora          | `sudo dnf install gcc-c++ cmake qt6-qtbase-devel`                        |
| Arch Linux      | `sudo pacman -S base-devel cmake qt6-base`                               |
| macOS           | `brew install cmake qt`                                                  |
| Windows         | siehe [Windows](#windows)                                                |

### Kompilieren

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/broadband
```

Liegt Qt nicht im Standardpfad (z. B. auf macOS oder Windows), gibst du den Pfad beim Konfigurieren mit an:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"      # macOS
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64       # Windows
```

Installieren (optional): `cmake --install build --prefix ~/.local`

## Benutzung

### Speedtest

1. `broadband` starten
2. Oben ist als Server `https://speed.cloudflare.com` voreingestellt. Die Adresse gilt für alle Tests.
3. Optional Messdauer (je Richtung), Anzahl paralleler Streams und Wiederholungen anpassen
4. **Test starten**

Die Ergebnisse erscheinen unten im Verlauf. Verlauf und Einstellungen bleiben zwischen den Starts gespeichert.

### Bandbreite im LAN messen

Auf **Rechner A** (Server):

```bash
broadband --server            # headless, Port 8080
broadband --server -p 9000    # anderer Port
```

Alternativ startest du den Server in der GUI im Tab **LAN-Server**. Dort werden auch die Adressen angezeigt, unter denen der Rechner erreichbar ist.

Auf **Rechner B** (Client) trägst du als Server `http://<IP-von-A>:8080` ein und startest den Test. Das geht auch direkt beim Start:

```bash
broadband --url http://192.168.1.20:8080
```

> Die Firewall auf Rechner A muss eingehende Verbindungen auf dem Port zulassen, z. B. mit `sudo ufw allow 8080/tcp`.

### Kommandozeile

```
Usage: broadband [options]

  -h, --help              Hilfe anzeigen
  -v, --version           Version anzeigen
  -s, --server            Nur den LAN-Testserver starten (ohne GUI)
  -p, --port <port>       Port des Testservers (Standard: 8080)
  -u, --url <url>         Server-Adresse für die Messung bzw. die in der GUI vorausgewählte
  -r, --run               Speedtest ohne GUI ausführen und das Ergebnis ausgeben
  -l, --load <stufen>     Lasttest ohne GUI, z. B. 1,2,4,8
  -d, --duration <s>      Messdauer je Richtung bzw. je Stufe (Standard: 10)
  -n, --streams <n>       Parallele Verbindungen beim Speedtest (Standard: 4, max. 32)
  --upload                Lasttest in Upload-Richtung
  --contract <mbit>       Vertragliche Bandbreite für die Auswertung des Lasttests
  --ip <4|6>              Nur IPv4 oder nur IPv6 verwenden
  -f, --format <format>   text, json oder csv (Standard: text)
  --no-header             Bei CSV keine Kopfzeile ausgeben
  --no-save               Ergebnis nicht im Verlauf speichern
```

### Proxy

broadband übernimmt die Proxy-Einstellungen des Systems, unter Linux also `http_proxy`, `https_proxy` und `no_proxy`. Private Adressen (`localhost`, `10.x`, `172.16–31.x`, `192.168.x`, `fe80::`, `fc00::/7`) werden immer direkt angesprochen, damit LAN-Tests nicht über einen Firmen-Proxy laufen. Bei Hostnamen im LAN hilft ein Eintrag in `no_proxy`.

## Tests

Die Unit-Tests starten einen lokalen Testserver und prüfen die Server-Endpunkte samt Idle-Timeout und Verbindungslimit, einen kompletten Speedtest inklusive Ping unter Last, mehr als 6 Streams, eine erzwungene IP-Version, den Lasttest mit mehreren Stufen, darunter mehr als 6 parallele Clients, jeweils in Download- und Upload-Richtung, das Abbrechen und den Fall, dass keine Verbindung zustande kommt. Ein absichtlich unzuverlässiger Server bricht Downloads mittendrin ab. Damit prüfen die Tests, dass Speedtest und Lasttest einzelne Abbrüche überstehen und bei dauerhaften Fehlern aufgeben. `tst_history` prüft den Verlauf (Speichern, Einlesen, beschädigte Zeilen, CSV) und die Vertragsprüfung mit allen drei Kriterien und dem Messprotokoll. `tst_report` prüft die Statistik (Median, Ausreißer, Trend), die Auswertungstexte und die erzeugte Excel-Datei bis hinunter zu ZIP-Prüfsummen und gültigem XML. Sie werden automatisch mitgebaut, wenn Qt6 *Test* installiert ist. Unter Debian und Ubuntu ist das in `qt6-base-dev` enthalten.

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Mit `-DBROADBAND_BUILD_TESTS=OFF` lassen sie sich abschalten.

## Projektstruktur

```
.
├── CMakeLists.txt
├── cmake/mingw-w64-x86_64.cmake  Toolchain für den Windows-Cross-Build
├── scripts/package-windows.sh    Windows-ZIP mit allen benötigten DLLs erzeugen
├── src/
│   ├── main.cpp              Einstieg, Optionen, GUI-, Server- oder Kommandozeilen-Modus
│   ├── Cli.{h,cpp}           Speedtest und Lasttest ohne GUI (Text, JSON, CSV)
│   ├── SpeedTest.{h,cpp}     Speedtest: Latenz, Download, Upload
│   ├── LoadTest.{h,cpp}      Bandbreite unter Last: Stufen mit N gleichzeitigen Clients
│   ├── HttpCommon.{h,cpp}    Gemeinsame HTTP-Helfer (Requests, Proxy, IP-Version, Serverinfo, Testdaten)
│   ├── History.{h,cpp}       Dauerhafter Verlauf (JSON Lines) und CSV-Format
│   ├── ContractCheck.{h,cpp} Vorprüfung nach den Kriterien der Bundesnetzagentur
│   ├── Statistics.h          Median, Streuung, Ausreißer und Trend einer Messreihe
│   ├── Report.{h,cpp}        Auswertung in Klartext und Aufbau der Excel-Arbeitsmappe
│   ├── XlsxWriter.{h,cpp}    Minimaler .xlsx-Writer (ZIP, Formate, Diagramme) ohne Zusatzbibliothek
│   ├── TestServer.{h,cpp}    Minimaler HTTP/1.1-Server für LAN-Tests (QTcpServer)
│   ├── MainWindow.{h,cpp}    Hauptfenster mit Speedtest-, Last- und Server-Tab
│   ├── ThroughputChart.{h,cpp}  Live-Diagramm (QPainter, ohne Qt Charts)
│   ├── broadband.rc.in       Versionsinfo für die Windows-exe
│   └── Format.h              Zahlenformatierung
├── tests/
│   ├── tst_speedtest.cpp     Tests für Messung und Testserver
│   ├── tst_history.cpp       Tests für Verlauf, CSV und Vertragsprüfung
│   └── tst_report.cpp        Tests für Statistik, Auswertung und Excel-Export
└── .github/workflows/build.yml  CI: Build und Tests unter Ubuntu und Windows
```

`SpeedTest`, `LoadTest`, `TestServer`, Verlauf, Vertragsprüfung und der Excel-Export bilden die Bibliothek `broadband_core`, die von App und Tests gemeinsam genutzt wird. Die Engine hängt nicht von Qt Widgets ab. Deshalb läuft der Kommandozeilen-Modus mit einer `QCoreApplication`, ganz ohne Display.

## Hinweise zur Messgenauigkeit

- Ergebnisse schwanken naturgemäß. Den besten Wert bekommst du **per Kabel** und ohne andere Downloads nebenher.
- Bei sehr schnellen Anschlüssen (≥ 1 Gbit/s) helfen mehr Streams, z. B. 8 bis 16 (maximal 32), und eine längere Messdauer.
- Der Upload wird über den Sendefortschritt gemessen. Hinter einem puffernden Proxy kann der Wert deshalb etwas zu hoch ausfallen.
- Der Ping ist ein **HTTP-Ping** (Request/Response über eine bestehende Verbindung). Er liegt etwas über dem ICMP-`ping`, dafür braucht er keine Root-Rechte.
- Die Cloudflare-Endpunkte sind nicht offiziell dokumentiert. Bitte nutze sie in Maßen und nicht für Dauermessungen im Minutentakt. Für lange Messreihen eine Pause von einigen Minuten einstellen.

## Sicherheit und Datenschutz

Das Programm ist bewusst klein gehalten und lässt sich vollständig prüfen:

- **Netzwerk:** Es gibt genau drei Arten von Anfragen, ausschließlich an den eingestellten Server (Standard: `speed.cloudflare.com`): `GET /__down?bytes=0` (Ping), `GET /__down?bytes=25000000` (Download), `POST /__up` (Upload). Mit erzwungener IP-Version kommt eine DNS-Abfrage für diesen Server hinzu. Es gibt keine Telemetrie, keine Update-Prüfung und keine weiteren Hosts. Der Server sieht dabei, wie bei jedem Speedtest, deine IP-Adresse und den User-Agent `broadband/<version>`. Rechenzentrum, IP und Provider (AS-Nummer) liest das Programm aus den `cf-meta-*`-Headern der Antworten, es gibt dafür keine zusätzliche Anfrage.
- **Downloads** landen nur im Arbeitsspeicher, werden gezählt und sofort verworfen. Es wird **nichts auf die Festplatte geschrieben und nichts ausgeführt**.
- **Uploads** bestehen aus zufällig erzeugten Daten. Es werden nie Dateien oder andere Daten von deinem Rechner gesendet.
- **Auf der Festplatte** speichert das Programm nur seine Einstellungen (Server, Dauer, Streams, Wiederholungen, Stufen, Vertrag, Port, zuletzt genutzter Export-Ordner) über `QSettings`: unter Linux in `~/.config/broadband/broadband.conf`, unter Windows in der Registry unter `HKCU\Software\broadband`. Dazu kommt der [Verlauf](#verlauf) mit den Messwerten, Zeitpunkten, Server-Adressen, dem Rechenzentrum und der IP-Version. Deine öffentliche IP-Adresse wird darin **nicht** gespeichert. Excel-Berichte entstehen nur, wenn du sie über **Excel-Export …** ausdrücklich an einem selbst gewählten Ort speicherst. Sie enthalten dieselben Daten wie der Verlauf.
- Es werden **keine anderen Programme gestartet** (kein `QProcess`, kein `system()`).
- **LAN-Server:** Nur solange er ausdrücklich gestartet ist, lauscht er auf allen Netzwerkschnittstellen und ohne Passwort. Er liefert ausschließlich Zufallsdaten und liest nichts von der Platte. Verbindungen ohne Datenverkehr schließt er nach 30 Sekunden, und mehr als 256 gleichzeitige Verbindungen lehnt er ab. In jeder Antwort teilt er dem Client dessen IP-Adresse mit (`cf-meta-ip`, wie Cloudflare). Jemand im selben Netz könnte mit dem Server aber Bandbreite verbrauchen. Nutze ihn deshalb nur in vertrauenswürdigen Netzen und beende ihn nach dem Test.

## Lizenz

[MIT](LICENSE)
