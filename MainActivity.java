package com.ilhamaqib.mxkingled;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.HashSet;
import java.util.List;
import java.util.Queue;
import java.util.Set;
import java.util.UUID;

public class MainActivity extends Activity {

    private static final int REQ_BLE_PERMS = 101;
    private static final int REQ_ENABLE_BT = 102;

    private static final UUID SERVICE_UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID CHAR_UUID_RX = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID CHAR_UUID_TX = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID CCCD_UUID    = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private WebView webView;
    private BluetoothAdapter btAdapter;
    private BluetoothLeScanner btScanner;
    private BluetoothGatt bluetoothGatt;
    private BluetoothGattCharacteristic rxChar;
    private BluetoothGattCharacteristic txChar;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Set<String> foundAddresses = new HashSet<>();
    private final Queue<String> writeQueue = new ArrayDeque<>();

    private boolean isScanning = false;
    private boolean isWriting = false;
    private boolean pendingScanAfterPerm = false;
    private String pendingConnectAddress = null;
    private String connectedName = "";

    private final Runnable stopScanRunnable = this::stopScanInternal;

    private final Runnable writeWatchdog = () -> {
        synchronized (writeQueue) {
            isWriting = false;
        }
        drainWriteQueue();
    };

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().setStatusBarColor(0xFF0B0C10);
        getWindow().setNavigationBarColor(0xFF0B0C10);

        BluetoothManager btManager = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        if (btManager != null) {
            btAdapter = btManager.getAdapter();
        }

        webView = new WebView(this);
        webView.setBackgroundColor(0xFF0B0C10);
        WebSettings ws = webView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setAllowFileAccess(true);
        ws.setCacheMode(WebSettings.LOAD_NO_CACHE);
        webView.setWebViewClient(new WebViewClient());
        webView.addJavascriptInterface(new BleJsBridge(), "AndroidBLE");

        setContentView(webView);
        webView.loadUrl("file:///android_asset/index.html");
    }

    @Override
    protected void onDestroy() {
        stopScanInternal();
        disconnectInternal();
        super.onDestroy();
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        evalJs("if(typeof handleAndroidBack==='function' && handleAndroidBack()){}else{AndroidBLE.exitApp();}");
    }

    // ================= PERMISSIONS =================
    private boolean hasBlePermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
        } else {
            return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
        }
    }

    private void requestBlePermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            requestPermissions(new String[]{
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            }, REQ_BLE_PERMS);
        } else {
            requestPermissions(new String[]{
                Manifest.permission.ACCESS_FINE_LOCATION
            }, REQ_BLE_PERMS);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_BLE_PERMS) {
            boolean allGranted = grantResults.length > 0;
            for (int r : grantResults) {
                if (r != PackageManager.PERMISSION_GRANTED) {
                    allGranted = false;
                    break;
                }
            }
            if (allGranted) {
                if (pendingConnectAddress != null) {
                    String addr = pendingConnectAddress;
                    pendingConnectAddress = null;
                    connectInternal(addr);
                } else if (pendingScanAfterPerm) {
                    pendingScanAfterPerm = false;
                    startScanInternal();
                }
            } else {
                pendingScanAfterPerm = false;
                pendingConnectAddress = null;
                sendStatus("error", "Izin Bluetooth ditolak. Aktifkan di Pengaturan HP.");
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ENABLE_BT) {
            if (resultCode == RESULT_OK) {
                if (pendingConnectAddress != null) {
                    String addr = pendingConnectAddress;
                    pendingConnectAddress = null;
                    connectInternal(addr);
                } else if (pendingScanAfterPerm) {
                    pendingScanAfterPerm = false;
                    startScanInternal();
                }
            } else {
                pendingScanAfterPerm = false;
                pendingConnectAddress = null;
                sendStatus("error", "Bluetooth harus dinyalakan terlebih dahulu.");
            }
        }
    }

    // ================= BLE SCANNING =================
    @SuppressLint("MissingPermission")
    private void startScanInternal() {
        if (btAdapter == null) {
            sendStatus("error", "HP ini tidak mendukung Bluetooth BLE.");
            return;
        }
        if (!hasBlePermissions()) {
            pendingScanAfterPerm = true;
            requestBlePermissions();
            return;
        }
        if (!btAdapter.isEnabled()) {
            pendingScanAfterPerm = true;
            Intent enableBtIntent = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE);
            startActivityForResult(enableBtIntent, REQ_ENABLE_BT);
            return;
        }

        stopScanInternal();
        btScanner = btAdapter.getBluetoothLeScanner();
        if (btScanner == null) {
            sendStatus("error", "Scanner BLE tidak tersedia.");
            return;
        }

        foundAddresses.clear();
        isScanning = true;
        sendStatus("scanning", "Memindai ESP32 MX King...");

        ScanSettings settings = new ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build();

        btScanner.startScan(null, settings, scanCallback);
        mainHandler.postDelayed(stopScanRunnable, 10000);
    }

    @SuppressLint("MissingPermission")
    private void stopScanInternal() {
        mainHandler.removeCallbacks(stopScanRunnable);
        if (isScanning && btScanner != null && btAdapter != null && btAdapter.isEnabled() && hasBlePermissions()) {
            try {
                btScanner.stopScan(scanCallback);
            } catch (Exception ignored) { }
        }
        if (isScanning) {
            isScanning = false;
            evalJs("if(typeof onBleScanStopped==='function') onBleScanStopped();");
        }
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @SuppressLint("MissingPermission")
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice dev = result.getDevice();
            if (dev == null) return;
            String addr = dev.getAddress();
            if (addr == null || foundAddresses.contains(addr)) return;

            String name = dev.getName();
            boolean hasTargetService = false;
            if (result.getScanRecord() != null) {
                if ((name == null || name.isEmpty()) && result.getScanRecord().getDeviceName() != null) {
                    name = result.getScanRecord().getDeviceName();
                }
                List<ParcelUuid> uuids = result.getScanRecord().getServiceUuids();
                if (uuids != null) {
                    for (ParcelUuid pu : uuids) {
                        if (SERVICE_UUID.equals(pu.getUuid())) {
                            hasTargetService = true;
                            break;
                        }
                    }
                }
            }

            boolean isTarget = hasTargetService || (name != null && name.toLowerCase().contains("mx_king"));
            if (!isTarget && (name == null || name.trim().isEmpty())) {
                return;
            }

            foundAddresses.add(addr);
            final String displayName = (name == null || name.trim().isEmpty()) ? "ESP32 BLE" : name.trim();
            final int rssi = result.getRssi();
            final boolean targetFlag = isTarget;

            evalJs("if(typeof onBleDeviceFound==='function') onBleDeviceFound("
                + quoteJs(displayName) + "," + quoteJs(addr) + "," + rssi + "," + targetFlag + ");");
        }

        @Override
        public void onScanFailed(int errorCode) {
            isScanning = false;
            sendStatus("error", "Gagal scan BLE (kode " + errorCode + ")");
        }
    };

    // ================= BLE CONNECTION =================
    @SuppressLint("MissingPermission")
    private void connectInternal(String address) {
        if (btAdapter == null) {
            sendStatus("error", "Bluetooth tidak tersedia.");
            return;
        }
        if (!hasBlePermissions()) {
            pendingConnectAddress = address;
            requestBlePermissions();
            return;
        }
        if (!btAdapter.isEnabled()) {
            pendingConnectAddress = address;
            Intent enableBtIntent = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE);
            startActivityForResult(enableBtIntent, REQ_ENABLE_BT);
            return;
        }

        stopScanInternal();
        disconnectInternal();

        try {
            BluetoothDevice device = btAdapter.getRemoteDevice(address);
            connectedName = (device.getName() != null && !device.getName().isEmpty()) ? device.getName() : address;
            sendStatus("connecting", "Menyambungkan ke " + connectedName + "...");
            bluetoothGatt = device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
        } catch (Exception e) {
            sendStatus("error", "Alamat perangkat tidak valid.");
        }
    }

    @SuppressLint("MissingPermission")
    private void disconnectInternal() {
        mainHandler.removeCallbacks(writeWatchdog);
        synchronized (writeQueue) {
            writeQueue.clear();
            isWriting = false;
        }
        rxChar = null;
        txChar = null;
        if (bluetoothGatt != null) {
            try {
                if (hasBlePermissions()) {
                    bluetoothGatt.disconnect();
                    bluetoothGatt.close();
                }
            } catch (Exception ignored) { }
            bluetoothGatt = null;
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                sendStatus("connecting", "Tersambung, membaca layanan BLE...");
                if (!gatt.requestMtu(128)) {
                    gatt.discoverServices();
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                disconnectInternal();
                sendStatus("disconnected", "Koneksi BLE terputus.");
            }
        }

        @SuppressLint("MissingPermission")
        @Override
        public void onMtuChanged(BluetoothGatt gatt, int mtu, int status) {
            gatt.discoverServices();
        }

        @SuppressLint("MissingPermission")
        @SuppressWarnings("deprecation")
        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                disconnectInternal();
                sendStatus("error", "Gagal membaca layanan BLE.");
                return;
            }
            BluetoothGattService svc = gatt.getService(SERVICE_UUID);
            if (svc == null) {
                disconnectInternal();
                sendStatus("error", "Perangkat bukan modul MX King LED (UUID tidak cocok).");
                return;
            }
            rxChar = svc.getCharacteristic(CHAR_UUID_RX);
            txChar = svc.getCharacteristic(CHAR_UUID_TX);
            if (rxChar == null || txChar == null) {
                disconnectInternal();
                sendStatus("error", "Karakteristik RX/TX tidak ditemukan.");
                return;
            }

            gatt.setCharacteristicNotification(txChar, true);
            BluetoothGattDescriptor cccd = txChar.getDescriptor(CCCD_UUID);
            if (cccd != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    gatt.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                } else {
                    cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    gatt.writeDescriptor(cccd);
                }
            } else {
                onReadyConnected();
            }
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt gatt, BluetoothGattDescriptor descriptor, int status) {
            if (CCCD_UUID.equals(descriptor.getUuid())) {
                onReadyConnected();
            }
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            mainHandler.removeCallbacks(writeWatchdog);
            synchronized (writeQueue) {
                isWriting = false;
            }
            drainWriteQueue();
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, byte[] value) {
            if (value != null && value.length > 0) {
                String msg = new String(value, StandardCharsets.UTF_8).trim();
                evalJs("if(typeof onBleData==='function') onBleData(" + quoteJs(msg) + ");");
            }
        }

        @SuppressWarnings("deprecation")
        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                byte[] value = characteristic.getValue();
                if (value != null && value.length > 0) {
                    String msg = new String(value, StandardCharsets.UTF_8).trim();
                    evalJs("if(typeof onBleData==='function') onBleData(" + quoteJs(msg) + ");");
                }
            }
        }
    };

    private void onReadyConnected() {
        String addr = (bluetoothGatt != null && bluetoothGatt.getDevice() != null)
            ? bluetoothGatt.getDevice().getAddress() : "";
        evalJs("if(typeof onBleConnected==='function') onBleConnected(" + quoteJs(connectedName) + "," + quoteJs(addr) + ");");
        enqueueCommand("GET");
    }

    private void enqueueCommand(String cmd) {
        if (bluetoothGatt == null || rxChar == null) return;
        synchronized (writeQueue) {
            if (writeQueue.size() >= 6) {
                writeQueue.poll();
            }
            writeQueue.offer(cmd);
        }
        drainWriteQueue();
    }

    @SuppressLint("MissingPermission")
    @SuppressWarnings("deprecation")
    private void drainWriteQueue() {
        String nextCmd;
        synchronized (writeQueue) {
            if (isWriting || writeQueue.isEmpty()) return;
            if (bluetoothGatt == null || rxChar == null || !hasBlePermissions()) {
                writeQueue.clear();
                return;
            }
            isWriting = true;
            nextCmd = writeQueue.poll();
        }

        if (nextCmd == null) return;
        byte[] payload = nextCmd.getBytes(StandardCharsets.UTF_8);
        int props = rxChar.getProperties();
        int writeType = ((props & BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0)
            ? BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            : BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT;

        boolean ok;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                int res = bluetoothGatt.writeCharacteristic(rxChar, payload, writeType);
                ok = (res == 0);
            } else {
                rxChar.setWriteType(writeType);
                rxChar.setValue(payload);
                ok = bluetoothGatt.writeCharacteristic(rxChar);
            }
        } catch (Exception e) {
            ok = false;
        }

        if (ok) {
            mainHandler.postDelayed(writeWatchdog, 250);
        } else {
            synchronized (writeQueue) {
                isWriting = false;
            }
            mainHandler.postDelayed(this::drainWriteQueue, 40);
        }
    }

    // ================= JS BRIDGE =================
    private class BleJsBridge {
        @JavascriptInterface
        public boolean isNativeApp() {
            return true;
        }

        @JavascriptInterface
        public void startScan() {
            mainHandler.post(() -> startScanInternal());
        }

        @JavascriptInterface
        public void stopScan() {
            mainHandler.post(() -> stopScanInternal());
        }

        @JavascriptInterface
        public void connectDevice(String address) {
            mainHandler.post(() -> connectInternal(address));
        }

        @JavascriptInterface
        public void disconnect() {
            mainHandler.post(() -> {
                disconnectInternal();
                sendStatus("disconnected", "Terputus dari perangkat.");
            });
        }

        @JavascriptInterface
        public void sendCommand(String cmd) {
            enqueueCommand(cmd);
        }

        @JavascriptInterface
        public void exitApp() {
            mainHandler.post(() -> finish());
        }
    }

    private void sendStatus(String state, String message) {
        evalJs("if(typeof onBleStatus==='function') onBleStatus(" + quoteJs(state) + "," + quoteJs(message) + ");");
    }

    private void evalJs(final String script) {
        mainHandler.post(() -> {
            if (webView != null) {
                webView.evaluateJavascript(script, null);
            }
        });
    }

    private String quoteJs(String s) {
        if (s == null) return "\"\"";
        String escaped = s.replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "");
        return "\"" + escaped + "\"";
    }
}
