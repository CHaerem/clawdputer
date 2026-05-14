// CoreBluetooth central that connects to the Cardputer's Nordic UART Service
// and exposes a line-oriented read/write interface.
//
// The buddy NUS UUIDs are used for the MVP; once `protocol/WIRE.md`'s clawd
// bridge service is implemented on the firmware side, swap the UUIDs here.

import CoreBluetooth
import Foundation

final class BLECentral: NSObject {
    // Advertising filter — the Cardputer only advertises NUS so Claude
    // Desktop's scan can find it. We scan by that too, then discover the
    // bridge service after connecting.
    static let advService    = CBUUID(string: "6e400001-b5a3-f393-e0a9-e50e24dcca9e")

    // clawd-bridge service — see protocol/WIRE.md
    static let bridgeService = CBUUID(string: "c1aedb01-1d0c-4adc-9b1a-c1aedb010000")
    static let bridgeRx      = CBUUID(string: "c1aedb01-1d0c-4adc-9b1a-c1aedb010001")
    static let bridgeTx      = CBUUID(string: "c1aedb01-1d0c-4adc-9b1a-c1aedb010002")

    private var manager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rxChar: CBCharacteristic?
    private var lineBuffer = Data()

    let namePrefix: String
    var onLine: ((String) -> Void)?
    var onReady: (() -> Void)?
    var onDisconnect: (() -> Void)?

    init(namePrefix: String) {
        self.namePrefix = namePrefix
        super.init()
        manager = CBCentralManager(delegate: self, queue: .main)
    }

    func send(_ line: String) {
        guard let p = peripheral, let rx = rxChar else {
            fputs("[ble] send dropped: not connected\n", stderr)
            return
        }
        guard let data = (line + "\n").data(using: .utf8) else { return }
        let mtu = p.maximumWriteValueLength(for: .withResponse)
        var i = 0
        while i < data.count {
            let end = min(i + mtu, data.count)
            p.writeValue(data.subdata(in: i..<end), for: rx, type: .withResponse)
            i = end
        }
    }

    var isReady: Bool { rxChar != nil }
}

extension BLECentral: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            print("[ble] powered on, scanning for \(namePrefix)*…")
            central.scanForPeripherals(withServices: [Self.advService], options: nil)
        case .poweredOff:
            fputs("[ble] bluetooth off\n", stderr)
        case .unauthorized:
            fputs("[ble] bluetooth permission denied — grant Bluetooth access to your terminal in System Settings → Privacy & Security → Bluetooth\n", stderr)
        case .unsupported:
            fputs("[ble] bluetooth unsupported on this host\n", stderr)
        case .resetting, .unknown:
            break
        @unknown default:
            break
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        let advName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let name = peripheral.name ?? advName ?? ""
        guard name.hasPrefix(namePrefix) else { return }
        print("[ble] found \(name) (rssi=\(RSSI)) — connecting")
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("[ble] connected, discovering services…")
        peripheral.discoverServices([Self.bridgeService])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        fputs("[ble] connect failed: \(error?.localizedDescription ?? "?")\n", stderr)
        central.scanForPeripherals(withServices: [Self.advService], options: nil)
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        print("[ble] disconnected — rescanning")
        self.peripheral = nil
        self.rxChar = nil
        lineBuffer.removeAll()
        onDisconnect?()
        central.scanForPeripherals(withServices: [Self.advService], options: nil)
    }
}

extension BLECentral: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let e = error {
            fputs("[ble] discoverServices: \(e.localizedDescription)\n", stderr)
            return
        }
        for svc in peripheral.services ?? [] where svc.uuid == Self.bridgeService {
            peripheral.discoverCharacteristics([Self.bridgeRx, Self.bridgeTx], for: svc)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        for c in service.characteristics ?? [] {
            if c.uuid == Self.bridgeRx { rxChar = c }
            if c.uuid == Self.bridgeTx { peripheral.setNotifyValue(true, for: c) }
        }
        if rxChar != nil {
            print("[ble] ready")
            onReady?()
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == Self.bridgeTx, let data = characteristic.value else { return }
        lineBuffer.append(data)
        while let nl = lineBuffer.firstIndex(of: 0x0A) {
            let lineData = lineBuffer.subdata(in: lineBuffer.startIndex..<nl)
            if let s = String(data: lineData, encoding: .utf8) {
                onLine?(s)
            }
            lineBuffer.removeSubrange(lineBuffer.startIndex...nl)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didWriteValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let e = error {
            fputs("[ble] write error: \(e.localizedDescription)\n", stderr)
        }
    }
}
