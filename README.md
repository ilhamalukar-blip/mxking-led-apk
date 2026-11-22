# MX King Smart LED V8.0 (Android Native BLE APK)

Paket proyek aplikasi Android (.apk) Bluetooth BLE Native (Android 14 Ready) + Firmware ESP32-C3 Super Mini.

## Langkah Compile Otomatis Jadi File .APK di Laptop (Tanpa Install Android Studio)
1. Ekstrak seluruh isi file ZIP ini ke satu folder di laptop.
2. Buka github.com, login, lalu buat repositori baru (Public atau Private).
3. Klik **"uploading an existing file"**, blok SEMUA file & folder hasil ekstrak (`Ctrl + A`), lalu seret masuk ke kotak upload (pastikan folder `.github` dan `app` ikut masuk).
4. Klik **Commit changes**.
5. Buka tab **Actions** di repositori GitHub, tunggu sekitar 1–2 menit sampai muncul tanda centang hijau.
6. Klik hasil build yang sudah hijau, gulir ke bagian **Artifacts**, lalu klik **MXKing_Smart_LED_V8_APK** untuk mengunduh.
7. Ekstrak file `MXKing_Smart_LED_V8.apk` dari dalamnya, kirim via WA Web ke HP ke-2, lalu instal!

## Firmware ESP32-C3
File firmware ada di folder `firmware_esp32/ESP32_C3_MX_King_BLE_V8.ino`.
Upload via Arduino IDE dengan pengaturan:
- Board: ESP32C3 Dev Module
- USB CDC On Boot: Enabled
- Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)

## Akun Login Aplikasi
- Nama: ILHAM 
- Password: 
