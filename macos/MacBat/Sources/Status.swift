import Foundation

/// `macbat status --json` (0.6+). Optional where older versions or other platforms may omit a field.
struct Status: Decodable {
    struct Conf: Decodable {
        let limit: Int
        let auto_discharge: Bool
        let topup: Bool?
    }
    struct Battery: Decodable {
        let percent: Int
        let state: String
        let ac: Bool?
        let voltage: Double?
        let current: Double?
        let power: Double?
        let temp_c: Double?
        let cycles: Int
        let full_mah: Int
        let design_mah: Int
        let health: Double?
        let health_reliable: Bool
        let time_to_empty_min: Int
        let time_to_full_min: Int
        let adapter_w: Int
    }
    struct Caps: Decodable {
        let hw_limit: String?
        let inhibit: String?
        let discharge: String?
    }
    struct Control: Decodable {
        let hw_limit: Int
        let charging_blocked: Int
        let adapter_off: Int
    }
    struct Fan: Decodable {
        let label: String
        let rpm: Int
        let min: Int
        let max: Int
        let target: Int
        let manual: Bool?
    }
    let version: String
    let platform: String
    let model: String
    let theme: String?
    let conf: Conf
    let battery: Battery
    let caps: Caps?
    let control: Control?
    let fans: [Fan]?
    let fan_mode: String?
    let fan_setting: String?
    let mode: String
    let control_source: String
    let conflict: String?
    let service: String
}
