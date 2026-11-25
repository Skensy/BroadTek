// SPDX-License-Identifier: MIT
// Advertise a read-only GATT service on the Mac. Read it once from the iPhone
// to confirm the incoming BLE link before testing the normal Wi-Fi menu.
import CoreBluetooth
import Foundation

final class IncomingLinkProbe: NSObject, CBPeripheralManagerDelegate {
    let serviceID = CBUUID(nsuuid: UUID())
    let characteristicID = CBUUID(string: "CAD50001-9BE0-4E86-A946-C8F7946C82D8")
    let payload = Data("Hotspot BLE OK".utf8)
    var manager: CBPeripheralManager!
    var published = false
    var localName: String { "Hotspot-" + serviceID.uuidString.prefix(4) }

    override init() {
        super.init()
        manager = CBPeripheralManager(delegate: self, queue: .main)
    }

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        switch peripheral.state {
        case .poweredOn:
            guard !published else { return }
            let characteristic = CBMutableCharacteristic(type: characteristicID, properties: .read,
                                                         value: nil, permissions: .readable)
            let service = CBMutableService(type: serviceID, primary: true)
            service.characteristics = [characteristic]
            published = true
            peripheral.add(service)
        case .unauthorized, .unsupported, .poweredOff:
            stop("Bluetooth non disponibile (stato \(peripheral.state.rawValue)).", code: 1)
        case .resetting where published:
            stop("Il controller Bluetooth si è riavviato.", code: 1)
        default:
            break
        }
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        if let error { stop("Registrazione del servizio fallita: \(error.localizedDescription)", code: 1) }
        peripheral.startAdvertising([CBAdvertisementDataLocalNameKey: localName,
                                     CBAdvertisementDataServiceUUIDsKey: [serviceID]])
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        if let error { stop("Avvio annuncio fallito: \(error.localizedDescription)", code: 1) }
        print("READY \(localName)  service=\(serviceID.uuidString)")
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        guard request.characteristic.uuid == characteristicID else {
            peripheral.respond(to: request, withResult: .attributeNotFound)
            return
        }
        guard (0...payload.count).contains(request.offset) else {
            peripheral.respond(to: request, withResult: .invalidOffset)
            return
        }
        request.value = Data(payload.dropFirst(request.offset))
        peripheral.respond(to: request, withResult: .success)
        print("READ OK \(ISO8601DateFormatter().string(from: Date()))")
    }

    func stop(_ message: String, code: Int32) -> Never {
        if manager.state == .poweredOn {
            manager.stopAdvertising()
            manager.removeAllServices()
        }
        print(message)
        exit(code)
    }
}

setbuf(stdout, nil)
let probe = IncomingLinkProbe()
let signals = [SIGINT, SIGTERM].map { number -> DispatchSourceSignal in
    signal(number, SIG_IGN)
    let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
    source.setEventHandler { probe.stop("Prova terminata.", code: 0) }
    source.resume()
    return source
}
DispatchQueue.main.asyncAfter(deadline: .now() + 900) { probe.stop("Sessione scaduta.", code: 0) }
withExtendedLifetime((probe, signals)) { RunLoop.main.run() }
