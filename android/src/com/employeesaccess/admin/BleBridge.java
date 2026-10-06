package com.employeesaccess.admin;

import android.app.AlertDialog;
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
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Base64;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.widget.ArrayAdapter;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.UUID;

/**
 * Native Bluetooth LE for the web page (window.AndroidBle).
 *
 * Every call carries a request id; the result goes back through
 * window.__androidBle.resolve(id, json) or .reject(id, name, message),
 * which android-bridge.js turns into Web Bluetooth promises. All state
 * lives on the main thread, and only one GATT operation runs at a time
 * (Android drops overlapping ones).
 */
class BleBridge {

    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final String KEY_ADDRESS = "ble_address";
    private static final String KEY_NAME = "ble_name";

    private static final long CONNECT_TIMEOUT_MS = 20000;
    private static final long BOND_TIMEOUT_MS = 90000;

    private final MainActivity activity;
    private final WebView web;
    private final Handler main = new Handler(Looper.getMainLooper());

    private BluetoothGatt gatt;
    private String gattAddress;
    private boolean connected;

    private int connectId;              // pending connect(), 0 = none
    private int opId;                   // pending GATT operation, 0 = none
    private Runnable retryAfterBond;    // repeats the operation once paired
    private BroadcastReceiver bondReceiver;

    private ScanCallback scanCallback;

    BleBridge(MainActivity activity, WebView web) {
        this.activity = activity;
        this.web = web;
    }

    /* ---------------- Results back to JavaScript ---------------- */

    private void js(String script) {
        main.post(() -> web.evaluateJavascript(script, null));
    }

    private void resolve(int id, String json) {
        js("window.__androidBle && __androidBle.resolve(" + id + "," + JSONObject.quote(json) + ")");
    }

    private void reject(int id, String name, String message) {
        js("window.__androidBle && __androidBle.reject(" + id + "," + JSONObject.quote(name) + ","
                + JSONObject.quote(message) + ")");
    }

    private BluetoothAdapter adapter() {
        BluetoothManager m = (BluetoothManager) activity.getSystemService(Context.BLUETOOTH_SERVICE);
        return m == null ? null : m.getAdapter();
    }

    /** Rejects and returns false when Bluetooth can't be used right now. */
    private boolean ready(int id) {
        BluetoothAdapter a = adapter();

        if (a == null) {
            reject(id, "NotSupportedError", "This phone has no Bluetooth.");
            return false;
        }
        if (!activity.hasBluetoothPermissions()) {
            activity.askBluetoothPermissions();
            reject(id, "NotAllowedError", "Allow the Bluetooth permission for this app, then try again.");
            return false;
        }
        if (!a.isEnabled()) {
            reject(id, "InvalidStateError", "Bluetooth is off. Turn it on and try again.");
            return false;
        }
        return true;
    }

    /* ---------------- requestDevice(): scan + chooser ---------------- */

    @JavascriptInterface
    public void requestDevice(int id, String serviceUuid) {
        main.post(() -> choose(id, serviceUuid));
    }

    private void choose(int id, String serviceUuid) {
        if (!ready(id)) {
            return;
        }

        BluetoothLeScanner scanner = adapter().getBluetoothLeScanner();
        if (scanner == null) {
            reject(id, "InvalidStateError", "Bluetooth is not ready yet. Try again.");
            return;
        }

        List<BluetoothDevice> found = new ArrayList<>();
        ArrayAdapter<String> labels = new ArrayAdapter<>(activity,
                android.R.layout.simple_list_item_1, new ArrayList<>());
        boolean[] done = { false };

        AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle("Searching for terminals…")
                .setAdapter(labels, (d, which) -> {
                    done[0] = true;
                    stopScan(scanner);
                    BluetoothDevice device = found.get(which);
                    String name = device.getName() != null ? device.getName() : "Terminal";
                    activity.prefs().edit()
                            .putString(KEY_ADDRESS, device.getAddress())
                            .putString(KEY_NAME, name)
                            .apply();
                    resolve(id, deviceJson(device.getAddress(), name));
                })
                .setNegativeButton("Cancel", null)
                .create();

        dialog.setOnDismissListener(d -> {
            stopScan(scanner);
            if (!done[0]) {
                done[0] = true;
                reject(id, "NotFoundError", "User cancelled the requestDevice() chooser.");
            }
        });

        scanCallback = new ScanCallback() {
            @Override
            public void onScanResult(int callbackType, ScanResult result) {
                main.post(() -> {
                    BluetoothDevice device = result.getDevice();
                    for (BluetoothDevice known : found) {
                        if (known.getAddress().equals(device.getAddress())) {
                            return;
                        }
                    }
                    String name = result.getScanRecord() != null && result.getScanRecord().getDeviceName() != null
                            ? result.getScanRecord().getDeviceName() : device.getName();
                    found.add(device);
                    labels.add((name != null ? name : "Unnamed terminal") + "\n" + device.getAddress()
                            + String.format(Locale.US, "  ·  %d dBm", result.getRssi()));
                    labels.notifyDataSetChanged();
                    dialog.setTitle("Choose a terminal");
                });
            }

            @Override
            public void onScanFailed(int errorCode) {
                main.post(() -> {
                    dialog.setTitle("Scan failed (" + errorCode + ")");
                });
            }
        };

        ScanFilter filter = new ScanFilter.Builder()
                .setServiceUuid(ParcelUuid.fromString(serviceUuid))
                .build();
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();

        dialog.show();
        scanner.startScan(Collections.singletonList(filter), settings, scanCallback);

        // Stop scanning after a while to save battery; the list stays open
        main.postDelayed(() -> {
            if (!done[0]) {
                stopScan(scanner);
                if (found.isEmpty()) {
                    dialog.setTitle("No terminal found nearby");
                }
            }
        }, 15000);
    }

    private void stopScan(BluetoothLeScanner scanner) {
        if (scanCallback != null) {
            try {
                scanner.stopScan(scanCallback);
            } catch (Exception ignored) {
                // Bluetooth turned off meanwhile
            }
            scanCallback = null;
        }
    }

    private static String deviceJson(String address, String name) {
        try {
            return new JSONObject().put("id", address).put("name", name).toString();
        } catch (JSONException e) {
            return "{}";
        }
    }

    /* ---------------- getDevices(): the terminal chosen last time ---------------- */

    @JavascriptInterface
    public void getDevices(int id) {
        String address = activity.prefs().getString(KEY_ADDRESS, null);
        String name = activity.prefs().getString(KEY_NAME, "Terminal");
        JSONArray list = new JSONArray();

        if (address != null) {
            try {
                list.put(new JSONObject(deviceJson(address, name)));
            } catch (JSONException ignored) {
                // cannot happen
            }
        }
        resolve(id, list.toString());
    }

    /* ---------------- Connection ---------------- */

    @JavascriptInterface
    public void connect(int id, String address) {
        main.post(() -> {
            if (!ready(id)) {
                return;
            }

            if (gatt != null && connected && address.equals(gattAddress)) {
                resolve(id, "null");
                return;
            }

            closeGatt();
            failPendingOp("NetworkError", "Reconnecting");

            BluetoothDevice device;
            try {
                device = adapter().getRemoteDevice(address);
            } catch (IllegalArgumentException e) {
                reject(id, "NotFoundError", "Unknown terminal address.");
                return;
            }

            connectId = id;
            gattAddress = address;
            gatt = device.connectGatt(activity, false, callback, BluetoothDevice.TRANSPORT_LE);

            main.postDelayed(() -> {
                if (connectId == id) {
                    connectId = 0;
                    closeGatt();
                    reject(id, "NetworkError", "Timed out connecting to the terminal. Is it on and nearby?");
                }
            }, CONNECT_TIMEOUT_MS);
        });
    }

    @JavascriptInterface
    public void disconnect(String address) {
        main.post(() -> {
            if (gatt != null && address.equals(gattAddress)) {
                gatt.disconnect();      // onConnectionStateChange reports it
            }
        });
    }

    private void closeGatt() {
        if (gatt != null) {
            gatt.close();
            gatt = null;
        }
        connected = false;
    }

    void shutdown() {
        main.removeCallbacksAndMessages(null);
        unregisterBond();
        closeGatt();
    }

    /* ---------------- Services and characteristics ---------------- */

    private BluetoothGattCharacteristic find(String service, String characteristic) {
        if (gatt == null || !connected) {
            return null;
        }
        BluetoothGattService s = gatt.getService(UUID.fromString(service));
        return s == null ? null : s.getCharacteristic(UUID.fromString(characteristic));
    }

    @JavascriptInterface
    public void hasService(int id, String address, String service) {
        main.post(() -> {
            if (gatt == null || !connected) {
                reject(id, "NetworkError", "GATT Server is disconnected.");
            } else if (gatt.getService(UUID.fromString(service)) == null) {
                reject(id, "NotFoundError", "The terminal has no such service.");
            } else {
                resolve(id, "null");
            }
        });
    }

    @JavascriptInterface
    public void hasCharacteristic(int id, String address, String service, String characteristic) {
        main.post(() -> {
            if (find(service, characteristic) == null) {
                reject(id, "NotFoundError", "The terminal has no such characteristic.");
            } else {
                resolve(id, "null");
            }
        });
    }

    /* ---------------- GATT operations (one at a time) ---------------- */

    private boolean beginOp(int id) {
        if (opId != 0) {
            reject(id, "InvalidStateError", "GATT operation already in progress.");
            return false;
        }
        opId = id;
        return true;
    }

    private void finishOp(String json) {
        int id = opId;
        opId = 0;
        retryAfterBond = null;
        if (id != 0) {
            resolve(id, json);
        }
    }

    private void failOp(String name, String message) {
        int id = opId;
        opId = 0;
        retryAfterBond = null;
        if (id != 0) {
            reject(id, name, message);
        }
    }

    private void failPendingOp(String name, String message) {
        if (opId != 0) {
            failOp(name, message);
        }
    }

    @JavascriptInterface
    public void read(int id, String address, String service, String characteristic) {
        main.post(() -> {
            BluetoothGattCharacteristic c = find(service, characteristic);
            if (c == null) {
                reject(id, "NetworkError", "GATT Server is disconnected.");
                return;
            }
            if (!beginOp(id)) {
                return;
            }
            retryAfterBond = () -> {
                if (gatt == null || !gatt.readCharacteristic(c)) {
                    failOp("NetworkError", "Read failed after pairing.");
                }
            };
            if (!gatt.readCharacteristic(c)) {
                failOp("NetworkError", "Read could not start.");
            }
        });
    }

    @JavascriptInterface
    @SuppressWarnings("deprecation")
    public void write(int id, String address, String service, String characteristic, String base64) {
        main.post(() -> {
            BluetoothGattCharacteristic c = find(service, characteristic);
            if (c == null) {
                reject(id, "NetworkError", "GATT Server is disconnected.");
                return;
            }
            if (!beginOp(id)) {
                return;
            }

            byte[] value = Base64.decode(base64, Base64.NO_WRAP);
            int type = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT;
            boolean started;

            if (Build.VERSION.SDK_INT >= 33) {
                started = gatt.writeCharacteristic(c, value, type) == BluetoothGatt.GATT_SUCCESS;
            } else {
                c.setWriteType(type);
                c.setValue(value);
                started = gatt.writeCharacteristic(c);
            }

            if (!started) {
                failOp("NetworkError", "Write could not start.");
            }
        });
    }

    @JavascriptInterface
    @SuppressWarnings("deprecation")
    public void startNotifications(int id, String address, String service, String characteristic) {
        main.post(() -> {
            BluetoothGattCharacteristic c = find(service, characteristic);
            BluetoothGattDescriptor d = c == null ? null : c.getDescriptor(CCCD);
            if (d == null) {
                reject(id, "NetworkError", "GATT Server is disconnected.");
                return;
            }
            if (!beginOp(id)) {
                return;
            }

            gatt.setCharacteristicNotification(c, true);
            byte[] enable = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE;
            boolean started;

            if (Build.VERSION.SDK_INT >= 33) {
                started = gatt.writeDescriptor(d, enable) == BluetoothGatt.GATT_SUCCESS;
            } else {
                d.setValue(enable);
                started = gatt.writeDescriptor(d);
            }

            if (!started) {
                failOp("NetworkError", "Could not subscribe to status updates.");
            }
        });
    }

    /* ---------------- Pairing ---------------- */

    private static boolean isAuthError(int status) {
        return status == BluetoothGatt.GATT_INSUFFICIENT_AUTHENTICATION
                || status == BluetoothGatt.GATT_INSUFFICIENT_ENCRYPTION
                || status == 137;   // GATT_AUTH_FAIL on some phones
    }

    /**
     * The terminal's characteristics need an encrypted, paired link. The
     * first protected read fails until the user types the 6-digit code
     * from the terminal's screen into Android's pairing prompt; then the
     * read is repeated.
     */
    private void waitForPairing(BluetoothDevice device) {
        if (retryAfterBond == null) {
            failOp("SecurityError", "Pairing required.");
            return;
        }

        unregisterBond();
        bondReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                BluetoothDevice d = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                if (d == null || !d.getAddress().equals(device.getAddress())) {
                    return;
                }
                int state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE);
                if (state == BluetoothDevice.BOND_BONDED) {
                    unregisterBond();
                    Runnable retry = retryAfterBond;
                    retryAfterBond = null;
                    main.postDelayed(retry, 300);
                } else if (state == BluetoothDevice.BOND_NONE) {
                    unregisterBond();
                    failOp("SecurityError", "Pairing failed or was cancelled. Check the code on the terminal and try again.");
                }
            }
        };
        activity.registerReceiver(bondReceiver, new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED));

        if (device.getBondState() == BluetoothDevice.BOND_NONE) {
            device.createBond();
        }

        main.postDelayed(() -> {
            if (bondReceiver != null) {
                unregisterBond();
                failOp("SecurityError", "Pairing timed out.");
            }
        }, BOND_TIMEOUT_MS);
    }

    private void unregisterBond() {
        if (bondReceiver != null) {
            try {
                activity.unregisterReceiver(bondReceiver);
            } catch (IllegalArgumentException ignored) {
                // already gone
            }
            bondReceiver = null;
        }
    }

    /* ---------------- GATT callbacks (binder thread -> main) ---------------- */

    private final BluetoothGattCallback callback = new BluetoothGattCallback() {

        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            main.post(() -> {
                if (g != gatt) {
                    g.close();
                    return;
                }

                if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                    // Leave the 7.5 ms default: the terminal shares its radio
                    // with Wi-Fi and loses fast links after ~5 s
                    g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_BALANCED);

                    // Room for the status JSON; then find the services
                    if (!g.requestMtu(185)) {
                        g.discoverServices();
                    }
                    return;
                }

                if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    boolean wasConnected = connected;
                    String address = gattAddress;
                    closeGatt();
                    unregisterBond();
                    failPendingOp("NetworkError", "The terminal disconnected.");

                    if (connectId != 0) {
                        int id = connectId;
                        connectId = 0;
                        reject(id, "NetworkError", "Couldn't connect to the terminal (status " + status + ").");
                    } else if (wasConnected) {
                        js("window.__androidBle && __androidBle.disconnected(" + JSONObject.quote(address) + ")");
                    }
                }
            });
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            main.post(() -> {
                if (g == gatt) {
                    g.discoverServices();
                }
            });
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            main.post(() -> {
                if (g != gatt || connectId == 0) {
                    return;
                }
                int id = connectId;
                connectId = 0;
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    connected = true;
                    resolve(id, "null");
                } else {
                    closeGatt();
                    reject(id, "NetworkError", "Couldn't read the terminal's services (status " + status + ").");
                }
            });
        }

        private void onRead(BluetoothGatt g, byte[] value, int status) {
            if (g != gatt) {
                return;
            }
            if (status == BluetoothGatt.GATT_SUCCESS) {
                finishOp(JSONObject.quote(Base64.encodeToString(value, Base64.NO_WRAP)));
            } else if (isAuthError(status)) {
                waitForPairing(g.getDevice());
            } else {
                failOp("NetworkError", "Read failed (status " + status + ").");
            }
        }

        @Override
        public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value, int status) {
            main.post(() -> onRead(g, value, status));
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            if (Build.VERSION.SDK_INT < 33) {
                byte[] value = c.getValue();
                main.post(() -> onRead(g, value, status));
            }
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            main.post(() -> {
                if (g != gatt) {
                    return;
                }
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    finishOp("null");
                } else {
                    failOp("NetworkError", "Write failed (status " + status + ").");
                }
            });
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            main.post(() -> {
                if (g != gatt) {
                    return;
                }
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    finishOp("null");
                } else {
                    failOp("NetworkError", "Subscribing failed (status " + status + ").");
                }
            });
        }

        private void onChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            if (g != gatt || value == null) {
                return;
            }
            js("window.__androidBle && __androidBle.notify(" + JSONObject.quote(gattAddress) + ","
                    + JSONObject.quote(c.getUuid().toString()) + ","
                    + JSONObject.quote(Base64.encodeToString(value, Base64.NO_WRAP)) + ")");
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            main.post(() -> onChanged(g, c, value));
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (Build.VERSION.SDK_INT < 33) {
                byte[] value = c.getValue();
                main.post(() -> onChanged(g, c, value));
            }
        }
    };
}
