# kode-findhub, in het kort (Nederlands)

Dit project zet je Kode Dot op het **Google Find Hub-netwerk**, ingebouwd in
kodeOS. Android-telefoons in de buurt melden dan versleuteld waar de Dot is.
Alleen jij kunt die plek lezen.

## Stappen

1. **kodeOS bouwen met de patch:**
   `git clone --recursive https://github.com/kodediy/KodeOS.git`, dan
   `./os/apply.sh KodeOS`, dan `idf.py build flash` in `KodeOS/`.
2. **Registreren:** `python3 tools/kodefind/kodefind.py --gfmt GoogleFindMyTools register --port /dev/ttyACM0`.
   Chrome vraagt één keer om je Google-login. De sleutel komt in
   `~/.kodefind/` en via USB op de Dot. **Maak een back-up van die sleutel.**
3. **Zoeken:** `python3 tools/kodefind/kodefind.py --gfmt GoogleFindMyTools locate`.
   Doe dit minstens elke 4 dagen, anders bewaart Google geen meldingen meer.

## Goed om te weten

* De Dot zendt alleen terwijl kodeOS draait en Bluetooth-host aan staat. Een
  draaiende app of een uitgeschakelde accu (geen USB) = geen baken.
* Je ziet de Dot niet als accessoire in de Find Hub-app en je kunt hem daar
  niet laten piepen. Locaties lopen via `kodefind`.
* Werkt de roterende modus niet, gebruik dan de statische modus (zie de
  README). Die werkt bewezen met GoogleFindMyTools, maar zendt altijd
  hetzelfde ID uit.
* Alleen je eigen Dot volgen. Android kan mensen waarschuwen voor een
  onbekende tracker die met ze meereist. Dat is de bedoeling.
* **Niet getest op echte hardware.** De rekenkern klopt byte voor byte met
  het algoritme van Google (GoogleFindMyTools). De kodeOS-build is gelukt.

## Channel Sounding (Bluetooth 6)

Niet mogelijk met de ingebouwde ESP32-C5, en ook niet met de huidige
Kode-modules (LoRa, Hacking-module met nRF24). Je hebt een RF-module met
een nRF54L15 nodig. Het ontwerp staat in [channel-sounding.md](channel-sounding.md).

## iPhone / Apple Find My

Ook ingebouwd, via OpenHaystack: `python3 tools/haystack/genkey.py --port ...`
(met `--watch` voor het horloge) maakt een sleutelpaar en zet de publieke
sleutel op het apparaat. iPhones melden dan versleuteld de plek bij Apple.
Je leest die met macless-haystack (Docker + Apple ID) of OpenHaystack (Mac).
**Niet in de Find My-app zelf**: "Ander item toevoegen" werkt alleen voor
MFi-gecertificeerde accessoires. Uitleg: [find-my.md](find-my.md).
