#!/usr/bin/env swift

@preconcurrency import CoreBluetooth
import AppKit
import Darwin
import Foundation

private let vhosService = CBUUID(string: "33613EB3-FFCA-42D1-83FA-A18F12B3F123")
private let commandCharacteristic = CBUUID(string: "B3D3279B-0244-4D54-A2AB-A1AB47A5FC0A")
private let streamCharacteristic = CBUUID(string: "265B90C0-A600-4659-BBBD-5CDA411C49CC")
private let statusCharacteristic = CBUUID(string: "BCB5699A-A9B4-49B8-B69B-D2DFF19B41A9")
private let factoryService = CBUUID(string: "FEE0")
private let securityRetryLimit = 30

private enum ProbeFailure: Error, CustomStringConvertible {
  case invalidFrame(String)

  var description: String {
    switch self {
    case .invalidFrame(let message): message
    }
  }
}

private enum CRC32C {
  static func checksum<T: DataProtocol>(_ bytes: T) -> UInt32 {
    var crc = UInt32.max
    for byte in bytes {
      crc ^= UInt32(byte)
      for _ in 0..<8 {
        crc = (crc >> 1) ^ ((crc & 1) == 1 ? 0x82F6_3B78 : 0)
      }
    }
    return ~crc
  }
}

private extension Data {
  mutating func appendLittleEndian<T: FixedWidthInteger>(_ value: T) {
    var value = value.littleEndian
    Swift.withUnsafeBytes(of: &value) { append(contentsOf: $0) }
  }

  func readUInt32LittleEndian(at offset: Int) -> UInt32 {
    UInt32(self[offset])
      | UInt32(self[offset + 1]) << 8
      | UInt32(self[offset + 2]) << 16
      | UInt32(self[offset + 3]) << 24
  }
}

private func handshakeFrame() throws -> Data {
  let payload = try JSONSerialization.data(withJSONObject: [
    "contract": "gateway.handshake",
    "contract_version": "1.0.0",
  ], options: [.sortedKeys])
  var header = Data("VHOS".utf8)
  header.append(contentsOf: [1, 0, 1, 0])
  header.appendLittleEndian(UInt32(payload.count))
  header.appendLittleEndian(UInt64(1))
  header.appendLittleEndian(UInt64(ProcessInfo.processInfo.systemUptime * 1_000_000))
  header.appendLittleEndian(CRC32C.checksum(payload))
  header.appendLittleEndian(CRC32C.checksum(header))
  return header + payload
}

private struct DecodedFrame {
  let type: UInt8
  let payload: Data
}

private func takeFrames(from buffer: inout Data) throws -> [DecodedFrame] {
  var frames: [DecodedFrame] = []
  while buffer.count >= 36 {
    guard buffer.prefix(4) == Data("VHOS".utf8) else {
      throw ProbeFailure.invalidFrame("invalid VHOS magic")
    }
    let payloadLength = Int(buffer.readUInt32LittleEndian(at: 8))
    guard payloadLength <= 1024 else {
      throw ProbeFailure.invalidFrame("payload exceeds firmware contract")
    }
    let frameLength = 36 + payloadLength
    guard buffer.count >= frameLength else { break }
    let frame = Data(buffer.prefix(frameLength))
    let expectedPayloadCRC = frame.readUInt32LittleEndian(at: 28)
    let expectedHeaderCRC = frame.readUInt32LittleEndian(at: 32)
    let payload = Data(frame[36..<frameLength])
    guard CRC32C.checksum(frame.prefix(32)) == expectedHeaderCRC else {
      throw ProbeFailure.invalidFrame("header CRC32C mismatch")
    }
    guard CRC32C.checksum(payload) == expectedPayloadCRC else {
      throw ProbeFailure.invalidFrame("payload CRC32C mismatch")
    }
    frames.append(DecodedFrame(type: frame[6], payload: payload))
    buffer.removeFirst(frameLength)
  }
  return frames
}

final class VHOSBLEProbe: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
  private var central: CBCentralManager!
  private var peripheral: CBPeripheral?
  private var command: CBCharacteristic?
  private var notificationBuffers: [CBUUID: Data] = [:]
  private var notifying: Set<CBUUID> = []
  private var notificationAttempts: [CBUUID: Int] = [:]
  private var handshakeSent = false
  private var handshakeAttempts = 0
  private var handshakeReceived = false
  private var healthReceived = false
  private var discoveredNames: Set<String> = []
  private let timeoutSeconds: TimeInterval

  init(timeoutSeconds: TimeInterval) {
    self.timeoutSeconds = timeoutSeconds
    super.init()
    print(
      "AUTHORIZATION state=\(CBManager.authorization.description) "
        + "bundle=\(Bundle.main.bundleIdentifier ?? "unbundled")"
    )
    central = CBCentralManager(delegate: self, queue: .main)
    DispatchQueue.main.asyncAfter(deadline: .now() + timeoutSeconds) { [weak self] in
      self?.finishFailure("timeout after \(Int(timeoutSeconds)) seconds")
    }
  }

  func centralManagerDidUpdateState(_ central: CBCentralManager) {
    print("STATE bluetooth=\(central.state.description)")
    guard central.state == .poweredOn else {
      if central.state == .unauthorized || central.state == .unsupported {
        finishFailure("Core Bluetooth is \(central.state.description)")
      }
      return
    }
    print("SCAN_START service=\(vhosService.uuidString)")
    central.scanForPeripherals(
      withServices: nil,
      options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
    )
  }

  func centralManager(
    _ central: CBCentralManager,
    didDiscover peripheral: CBPeripheral,
    advertisementData: [String: Any],
    rssi RSSI: NSNumber
  ) {
    let services = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
    let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
      ?? peripheral.name ?? "unnamed"
    let supported = services.contains(vhosService)
      || services.contains(factoryService)
      || name.localizedCaseInsensitiveContains("VHOS")
      || name.localizedCaseInsensitiveContains("WiCAN")
    if discoveredNames.insert("\(peripheral.identifier.uuidString):\(name)").inserted || supported {
      print(
        "DISCOVER name=\(name) id=\(peripheral.identifier.uuidString) rssi=\(RSSI) "
          + "services=\(services.map(\.uuidString).joined(separator: ",")) supported=\(supported)"
      )
    }
    guard supported, self.peripheral == nil else { return }
    self.peripheral = peripheral
    central.stopScan()
    peripheral.delegate = self
    print("CONNECT_START name=\(name)")
    central.connect(peripheral)
  }

  func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
    print("CONNECT_PASS id=\(peripheral.identifier.uuidString)")
    peripheral.discoverServices([vhosService, factoryService])
  }

  func centralManager(
    _ central: CBCentralManager,
    didFailToConnect peripheral: CBPeripheral,
    error: Error?
  ) {
    finishFailure("connect failed: \(error?.localizedDescription ?? "unknown error")")
  }

  func centralManager(
    _ central: CBCentralManager,
    didDisconnectPeripheral peripheral: CBPeripheral,
    error: Error?
  ) {
    if !handshakeReceived || !healthReceived {
      finishFailure("disconnected before contract verification: \(error?.localizedDescription ?? "no error")")
    }
  }

  func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
    if let error { return finishFailure("service discovery failed: \(error.localizedDescription)") }
    let services = peripheral.services ?? []
    print("SERVICES values=\(services.map { $0.uuid.uuidString }.joined(separator: ","))")
    guard let service = services.first(where: { $0.uuid == vhosService }) else {
      return finishFailure("VHOS service not found")
    }
    print("SERVICE_PASS uuid=\(service.uuid.uuidString)")
    peripheral.discoverCharacteristics(
      [commandCharacteristic, streamCharacteristic, statusCharacteristic],
      for: service
    )
  }

  func peripheral(
    _ peripheral: CBPeripheral,
    didDiscoverCharacteristicsFor service: CBService,
    error: Error?
  ) {
    if let error {
      return finishFailure("characteristic discovery failed: \(error.localizedDescription)")
    }
    let characteristics = service.characteristics ?? []
    print("CHARACTERISTICS values=\(characteristics.map { $0.uuid.uuidString }.joined(separator: ","))")
    command = characteristics.first(where: { $0.uuid == commandCharacteristic })
    guard command != nil else { return finishFailure("command characteristic not found") }
    guard let stream = characteristics.first(where: { $0.uuid == streamCharacteristic }) else {
      return finishFailure("stream characteristic not found")
    }
    guard let status = characteristics.first(where: { $0.uuid == statusCharacteristic }) else {
      return finishFailure("status characteristic not found")
    }
    print("CHARACTERISTICS_PASS")
    peripheral.setNotifyValue(true, for: stream)
    peripheral.setNotifyValue(true, for: status)
  }

  func peripheral(
    _ peripheral: CBPeripheral,
    didUpdateNotificationStateFor characteristic: CBCharacteristic,
    error: Error?
  ) {
    if let error {
      let value = error as NSError
      let retryable = value.domain == CBATTErrorDomain
        && (value.code == CBATTError.insufficientEncryption.rawValue
          || value.code == CBATTError.insufficientAuthentication.rawValue)
      let attempts = notificationAttempts[characteristic.uuid, default: 0]
      if retryable, attempts < securityRetryLimit {
        notificationAttempts[characteristic.uuid] = attempts + 1
        print(
          "NOTIFY_SECURITY_WAIT uuid=\(characteristic.uuid.uuidString) "
            + "attempt=\(attempts + 1) reason=\(error.localizedDescription)"
        )
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self, weak peripheral] in
          guard self != nil, let peripheral else { return }
          peripheral.setNotifyValue(true, for: characteristic)
        }
        return
      }
      return finishFailure(
        "notification setup failed for \(characteristic.uuid.uuidString): \(error.localizedDescription)"
      )
    }
    notificationAttempts[characteristic.uuid] = 0
    if characteristic.isNotifying { notifying.insert(characteristic.uuid) }
    print("NOTIFY uuid=\(characteristic.uuid.uuidString) active=\(characteristic.isNotifying)")
    guard notifying.contains(streamCharacteristic), notifying.contains(statusCharacteristic) else {
      return
    }
    sendHandshakeIfReady()
  }

  func peripheral(
    _ peripheral: CBPeripheral,
    didWriteValueFor characteristic: CBCharacteristic,
    error: Error?
  ) {
    if let error {
      let value = error as NSError
      let retryable = value.domain == CBATTErrorDomain
        && (value.code == CBATTError.insufficientEncryption.rawValue
          || value.code == CBATTError.insufficientAuthentication.rawValue)
      if retryable, handshakeAttempts < securityRetryLimit {
        print(
          "HANDSHAKE_SECURITY_WAIT attempt=\(handshakeAttempts) "
            + "reason=\(error.localizedDescription)"
        )
        handshakeSent = false
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
          self?.sendHandshakeIfReady()
        }
      } else {
        finishFailure("handshake write failed: \(error.localizedDescription)")
      }
    } else {
      print("HANDSHAKE_WRITE_PASS bytes=\(handshakeFrameByteCount)")
    }
  }

  private var handshakeFrameByteCount = 0

  private func sendHandshakeIfReady() {
    guard !handshakeSent, let peripheral, let command else { return }
    do {
      let frame = try handshakeFrame()
      let maximum = peripheral.maximumWriteValueLength(for: .withResponse)
      guard frame.count <= maximum else {
        return finishFailure("handshake frame exceeds one reliable-write packet")
      }
      handshakeSent = true
      handshakeAttempts += 1
      handshakeFrameByteCount = frame.count
      print(
        "HANDSHAKE_WRITE_START attempt=\(handshakeAttempts) bytes=\(frame.count) maximum=\(maximum)"
      )
      peripheral.writeValue(frame, for: command, type: .withResponse)
    } catch {
      finishFailure("unable to encode handshake: \(error)")
    }
  }

  func peripheral(
    _ peripheral: CBPeripheral,
    didUpdateValueFor characteristic: CBCharacteristic,
    error: Error?
  ) {
    if let error {
      return finishFailure(
        "notification failed for \(characteristic.uuid.uuidString): \(error.localizedDescription)"
      )
    }
    guard let value = characteristic.value else { return }
    notificationBuffers[characteristic.uuid, default: Data()].append(value)
    do {
      var buffer = notificationBuffers[characteristic.uuid, default: Data()]
      let frames = try takeFrames(from: &buffer)
      notificationBuffers[characteristic.uuid] = buffer
      for frame in frames {
        let json = String(data: frame.payload, encoding: .utf8) ?? "<non-UTF8>"
        print("FRAME_PASS type=\(frame.type) payload=\(json)")
        if frame.type == 1 { handshakeReceived = true }
        if frame.type == 4 { healthReceived = true }
      }
      if handshakeReceived && healthReceived {
        print("VHOS_BLE_PROBE_PASS handshake=true health=true")
        finish(exitCode: EXIT_SUCCESS)
      }
    } catch {
      finishFailure("frame decode failed: \(error)")
    }
  }

  private func finishFailure(_ reason: String) {
    print("VHOS_BLE_PROBE_FAIL reason=\(reason)")
    finish(exitCode: EXIT_FAILURE)
  }

  private func finish(exitCode: Int32) {
    central?.stopScan()
    if let peripheral { central?.cancelPeripheralConnection(peripheral) }
    fflush(stdout)
    Darwin.exit(exitCode)
  }
}

private extension CBManagerState {
  var description: String {
    switch self {
    case .unknown: "unknown"
    case .resetting: "resetting"
    case .unsupported: "unsupported"
    case .unauthorized: "unauthorized"
    case .poweredOff: "poweredOff"
    case .poweredOn: "poweredOn"
    @unknown default: "future(\(rawValue))"
    }
  }
}

private extension CBManagerAuthorization {
  var description: String {
    switch self {
    case .notDetermined: "notDetermined"
    case .restricted: "restricted"
    case .denied: "denied"
    case .allowedAlways: "allowedAlways"
    @unknown default: "future(\(rawValue))"
    }
  }
}

let arguments = Array(CommandLine.arguments.dropFirst())
let timeout = arguments.first.flatMap(TimeInterval.init) ?? 20
if arguments.count > 1 {
  _ = freopen(arguments[1], "a", stdout)
  _ = freopen(arguments[1], "a", stderr)
  setbuf(stdout, nil)
  setbuf(stderr, nil)
}
print("VHOS_BLE_PROBE_START timeout_seconds=\(Int(timeout))")
let application = NSApplication.shared
application.setActivationPolicy(.accessory)
application.finishLaunching()
application.activate(ignoringOtherApps: false)
let probeWindow = NSWindow(
  contentRect: NSRect(x: 0, y: 0, width: 460, height: 160),
  styleMask: [.titled, .closable],
  backing: .buffered,
  defer: false
)
probeWindow.title = "VHOS BLE Probe"
let probeLabel = NSTextField(
  wrappingLabelWithString:
    "Scanning for the VHOS ESP32, then verifying GATT services, encrypted handshake, and live health evidence."
)
probeLabel.frame = NSRect(x: 24, y: 38, width: 412, height: 82)
probeWindow.contentView?.addSubview(probeLabel)
probeWindow.center()
probeWindow.makeKeyAndOrderFront(nil)
let probe = VHOSBLEProbe(timeoutSeconds: timeout)
withExtendedLifetime((probe, probeWindow)) {
  application.run()
}
