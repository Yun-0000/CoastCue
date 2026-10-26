import Foundation

enum GatewayMessageError: Error, LocalizedError, Equatable {
    case malformedFragment
    case unsupportedFrameFormat
    case invalidCRC
    case invalidJSON
    case oversizedUnframedMessage

    var errorDescription: String? {
        switch self {
        case .malformedFragment: String(localized: "Malformed gateway message")
        case .unsupportedFrameFormat: String(localized: "Unsupported gateway message format")
        case .invalidCRC: String(localized: "Gateway message checksum failed")
        case .invalidJSON: String(localized: "Gateway message could not be decoded")
        case .oversizedUnframedMessage: String(localized: "Gateway message exceeds the 160-byte unframed limit")
        }
    }
}

struct CRC32 {
    static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = (crc & 1) == 1 ? (crc >> 1) ^ 0xEDB8_8320 : crc >> 1 }
        }
        return crc ^ 0xFFFF_FFFF
    }
}

struct GatewayMessageFramer {
    static let unframedMessageLimit = 160
    static let maximumMessageBytes = 8192
    static let maximumFragments = 2048 // 8192 bytes at the minimum 4-byte ATT fragment payload.
    static let maximumPartialMessages = 8

    private struct Partial {
        var expected: Int
        var crc: UInt32
        var chunks: [Int: Data]
        var updated: Date
    }
    private var partials: [UInt32: Partial] = [:]
    let timeout: TimeInterval

    init(timeout: TimeInterval = 15) { self.timeout = timeout }

    mutating func ingest(_ data: Data, now: Date = Date()) throws -> Data? {
        purgeExpired(now: now)
        if data.starts(with: [0x53, 0x4D]), data.count < 16 {
            throw GatewayMessageError.malformedFragment
        }
        guard data.count >= 16 else { return try decodeUnframedMessage(data) }
        let bytes = [UInt8](data)
        guard bytes[0] == 0x53, bytes[1] == 0x4D else { return try decodeUnframedMessage(data) }
        guard bytes[2] == 1, bytes[3] == 1 else { throw GatewayMessageError.unsupportedFrameFormat }
        let messageID = value32(bytes, 4)
        let index = Int(value16(bytes, 8))
        let count = Int(value16(bytes, 10))
        guard count > 0, count <= Self.maximumFragments, index < count,
              data.count > 16, data.count <= Self.unframedMessageLimit + 16 else {
            partials[messageID] = nil
            throw GatewayMessageError.malformedFragment
        }
        if partials[messageID] == nil, partials.count >= Self.maximumPartialMessages,
           let oldest = partials.min(by: { $0.value.updated < $1.value.updated })?.key {
            partials[oldest] = nil
        }
        let crc = value32(bytes, 12)
        var partial = partials[messageID] ?? Partial(expected: count, crc: crc, chunks: [:], updated: now)
        let chunk = Data(data.dropFirst(16))
        guard partial.expected == count, partial.crc == crc,
              partial.chunks[index] == nil || partial.chunks[index] == chunk else {
            partials[messageID] = nil
            throw GatewayMessageError.malformedFragment
        }
        partial.chunks[index] = chunk
        guard partial.chunks.values.reduce(0, { $0 + $1.count }) <= Self.maximumMessageBytes else {
            partials[messageID] = nil
            throw GatewayMessageError.malformedFragment
        }
        partial.updated = now
        partials[messageID] = partial
        guard partial.chunks.count == count else { return nil }
        let full = (0..<count).reduce(into: Data()) { $0.append(partial.chunks[$1] ?? Data()) }
        partials[messageID] = nil
        guard CRC32.checksum(full) == crc else { throw GatewayMessageError.invalidCRC }
        return try decodeJSON(full)
    }

    private mutating func purgeExpired(now: Date) {
        partials = partials.filter { now.timeIntervalSince($0.value.updated) <= timeout }
    }

    private func decodeUnframedMessage(_ data: Data) throws -> Data {
        guard data.count <= Self.unframedMessageLimit else { throw GatewayMessageError.oversizedUnframedMessage }
        return try decodeJSON(data)
    }

    private func decodeJSON(_ data: Data) throws -> Data {
        guard !data.isEmpty, (try? JSONSerialization.jsonObject(with: data)) != nil else { throw GatewayMessageError.invalidJSON }
        return data
    }

    private func value16(_ bytes: [UInt8], _ offset: Int) -> UInt16 { UInt16(bytes[offset]) | (UInt16(bytes[offset + 1]) << 8) }
    private func value32(_ bytes: [UInt8], _ offset: Int) -> UInt32 { UInt32(bytes[offset]) | (UInt32(bytes[offset + 1]) << 8) | (UInt32(bytes[offset + 2]) << 16) | (UInt32(bytes[offset + 3]) << 24) }
}

// Validate gaps as intervals. Runtime depends on page size, never the distance
// between journal IDs (a compacted journal can skip millions of IDs).
func journalPageIDsAreValid(
    _ ids: [Int64], after: Int64, first: Int64, last: Int64,
    truncatedBelow: Int64?, missing: [[Int64]]
) -> Bool {
    let maximumID: Int64 = 0x7FFF_FFFF
    guard (0...maximumID).contains(after), ids.count <= 20,
          ids.allSatisfy({ (1...maximumID).contains($0) }),
          (ids.first ?? 0) == first, (ids.last ?? after) == last,
          zip(ids, ids.dropFirst()).allSatisfy({ $0 < $1 }),
          missing.allSatisfy({ $0.count == 2 && $0[0] > 0 && $0[0] <= $0[1] && $0[1] <= maximumID })
    else { return false }
    var ranges = missing
    if let boundary = truncatedBelow {
        guard (1...maximumID).contains(boundary) else { return false }
        if boundary > 1 { ranges.append([1, boundary - 1]) }
    }
    ranges.sort { $0[0] < $1[0] }
    var expected = after + 1
    for id in ids {
        guard id >= expected else { return false }
        for range in ranges where expected < id && range[0] <= expected && range[1] >= expected {
            expected = min(id, range[1] + 1)
        }
        guard expected == id else { return false }
        expected = id + 1
    }
    return true
}
