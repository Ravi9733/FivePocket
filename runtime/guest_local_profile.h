// Local (offline) Xbox 360 user profile model for the native XAM import layer.
//
// Header-only, host-testable, C++17. No global mutable state; every public
// entry point is noexcept and reports failure through its return value.
// Persistence uses POSIX open/write/fsync/rename (atomic replace).
//
// Byte layouts below were derived by reading (not copying) Xenia canary:
//   src/xenia/kernel/xam/xam.h           X_USER_SIGNIN_INFO (40 bytes),
//                                        X_USER_READ_PROFILE_SETTINGS (8),
//                                        X_USER_GET_SIGNIN_INFO_*_XUID_ONLY
//   src/xenia/kernel/xam/user_profile.h  X_USER_PROFILE_SETTING (40 bytes),
//                                        X_USER_PROFILE_SETTING_SOURCE,
//                                        SignInState (1 = signed in locally)
//   src/xenia/kernel/xam/user_data.h     AttributeKey bitfield, X_USER_DATA
//                                        (16 bytes, alignas 8), data types,
//                                        kMaxUserDataSize = 1000
//   src/xenia/kernel/xam/user_settings.h setting keys, title-writable set,
//                                        option enums, default overrides
//   src/xenia/kernel/xam/user_settings.cc default-by-type, WriteToGuest
//   src/xenia/kernel/xam/xam_user.cc     read/write/sign-in/name semantics
//   src/xenia/kernel/kernel.h            IsOfflineXUID / IsOnlineXUID /
//                                        IsGuestXUID / IsTeamXUID
//   src/xenia/kernel/xam/user_profile.cc note: title 58410A1F rejects XUIDs
//                                        with (xuid & 0x00C0000000000000)
//
// X_USER_SIGNIN_INFO (big-endian, 40 bytes):
//   +0  u64 xuid            (zero when caller asked ONLINE_XUID_ONLY (1): a
//                            local profile has no online XUID)
//   +8  u32 info flags      (0: not Live-enabled, not a guest)
//   +12 u32 signin state    (0 none, 1 signed in locally, 2 signed in to Live)
//   +16 u32 guest number    (0)
//   +20 u32 sponsor index   (0)
//   +24 char name[16]       (<=15 ASCII + NUL, zero padded)
//
// X_USER_READ_PROFILE_SETTINGS header (big-endian, 8 bytes):
//   +0 u32 setting count    (after unknown ids are compacted out)
//   +4 u32 guest pointer to the X_USER_PROFILE_SETTING array (= buffer + 8)
// X_USER_PROFILE_SETTING (big-endian, 40 bytes, natural 8-byte alignment):
//   +0  u32 source          (0 no value, 1 default, 2 title, 3 denied)
//   +4  u32 padding (0)
//   +8  union { u32 user_index; u64 xuid; } -- read path writes xuid = -1
//       and then user_index over its first word, so +8 = user index and
//       +12 = 0xFFFFFFFF (xam_user.cc read path)
//   +16 u32 setting id (packed key)
//   +20 u32 padding (0)
//   +24 X_USER_DATA: +24 u8 type, +25..31 padding, +32 8-byte union:
//       int32/float/context: 4 BE bytes at +32, +36..39 zero
//       int64/double/datetime: 8 BE bytes at +32
//       wstring/binary: u32 byte size at +32, u32 guest pointer at +36
//       (wstring size counts the UTF-16BE NUL terminator)
// Variable data area follows the full (uncompacted) record array at
// buffer + 8 + 40 * requestedCount. Each non-empty wstring/binary value is
// copied to the cursor, then the cursor advances by min(key size, 1000).
// Empty binary values carry size 0 and pointer 0 and do not advance.
//
// Setting key: type bits 28..31, max byte size bits 16..27, id bits 0..15.
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace recomp::bootstrap::profile {

// ---------------------------------------------------------------------------
// Status codes (Win32 style, as XAM's DWORD-returning exports use).
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kErrorSuccess = 0;
// Local policy code: a title tried to write a system (dashboard-owned)
// setting. The real XAM return value for this case is not established.
inline constexpr std::uint32_t kErrorAccessDenied = 5;
// Local host I/O failure while persisting; not a value observed from XAM.
inline constexpr std::uint32_t kErrorWriteFault = 29;
inline constexpr std::uint32_t kErrorInvalidParameter = 87;
inline constexpr std::uint32_t kErrorInsufficientBuffer = 122;
// Local host storage is corrupt/unreadable. The registry should surface this
// (reject the import) instead of forwarding it to the title.
inline constexpr std::uint32_t kErrorStorageFault = 1392;

inline constexpr std::uint32_t kSigninStateNotSignedIn = 0;
inline constexpr std::uint32_t kSigninStateSignedInLocally = 1;
inline constexpr std::uint32_t kSigninStateSignedInToLive = 2;

// XamUserGetSigninInfo query flags (xam.h).
inline constexpr std::uint32_t kSigninInfoOnlineXuidOnly = 1;
inline constexpr std::uint32_t kSigninInfoOfflineXuidOnly = 2;

inline constexpr std::uint32_t kMaxUserDataSize = 1000;  // 0x3E8
inline constexpr std::uint32_t kMaxReadSettings = 32;    // xam_user.cc bound
// Local sanity bound for XamUserWriteProfileSettings (no XAM bound known).
inline constexpr std::uint32_t kMaxWriteSettings = 32;
inline constexpr std::uint32_t kSettingRecordSize = 40;
inline constexpr std::uint32_t kReadHeaderSize = 8;
inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

inline constexpr std::uint32_t kDashboardTitleId = 0xFFFE07D1u;

enum class DataType : std::uint8_t {
    Context = 0,
    Int32 = 1,
    Int64 = 2,
    Double = 3,
    WString = 4,
    Float = 5,
    Binary = 6,
    DateTime = 7,
    Null = 0xFF,
};

enum class Source : std::uint32_t {
    NoValue = 0,
    Default = 1,
    Title = 2,
    PermissionDenied = 3,
};

constexpr std::uint32_t settingKey(DataType type, std::uint32_t size, std::uint32_t id) {
    return (static_cast<std::uint32_t>(type) << 28) | ((size & 0xFFFu) << 16) | (id & 0xFFFFu);
}
constexpr std::uint32_t keyType(std::uint32_t key) { return key >> 28; }
constexpr std::uint32_t keySize(std::uint32_t key) { return (key >> 16) & 0xFFFu; }
constexpr std::uint32_t keyId(std::uint32_t key) { return key & 0xFFFFu; }
constexpr bool keyHasVariableData(std::uint32_t key) {
    return keyType(key) == static_cast<std::uint32_t>(DataType::WString) ||
           keyType(key) == static_cast<std::uint32_t>(DataType::Binary);
}
// Bytes a variable-size value may occupy; also the data-area slot stride.
constexpr std::uint32_t maxDataSize(std::uint32_t key) {
    return keySize(key) < kMaxUserDataSize ? keySize(key) : kMaxUserDataSize;
}
// Natural width of a scalar value inside the X_USER_DATA union, 0 otherwise.
constexpr std::uint32_t scalarWidth(std::uint32_t type) {
    switch (type) {
        case 0: case 1: case 5: return 4;  // context, int32, float
        case 2: case 3: case 7: return 8;  // int64, double, datetime
        default: return 0;
    }
}

namespace key {
inline constexpr std::uint32_t GamerType = 0x10040001u;
inline constexpr std::uint32_t GamerYAxisInversion = 0x10040002u;
inline constexpr std::uint32_t OptionControllerVibration = 0x10040003u;
inline constexpr std::uint32_t GamercardZone = 0x10040004u;
inline constexpr std::uint32_t GamercardRegion = 0x10040005u;
inline constexpr std::uint32_t GamercardCred = 0x10040006u;
inline constexpr std::uint32_t OptionVoiceMuted = 0x1004000Cu;
inline constexpr std::uint32_t OptionVoiceThruSpeakers = 0x1004000Du;
inline constexpr std::uint32_t OptionVoiceVolume = 0x1004000Eu;
inline constexpr std::uint32_t GamercardTitlesPlayed = 0x10040012u;
inline constexpr std::uint32_t GamercardAchievementsEarned = 0x10040013u;
inline constexpr std::uint32_t GamerDifficulty = 0x10040015u;
inline constexpr std::uint32_t GamerControlSensitivity = 0x10040018u;
inline constexpr std::uint32_t GamerPreferredColorFirst = 0x1004001Du;
inline constexpr std::uint32_t GamerPreferredColorSecond = 0x1004001Eu;
inline constexpr std::uint32_t GamerActionAutoAim = 0x10040022u;
inline constexpr std::uint32_t GamerActionAutoCenter = 0x10040023u;
inline constexpr std::uint32_t GamerActionMovementControl = 0x10040024u;
inline constexpr std::uint32_t GamerRaceTransmission = 0x10040026u;
inline constexpr std::uint32_t GamerRaceCameraLocation = 0x10040027u;
inline constexpr std::uint32_t GamerRaceBrakeControl = 0x10040028u;
inline constexpr std::uint32_t GamerRaceAcceleratorControl = 0x10040029u;
inline constexpr std::uint32_t GamercardTitleCredEarned = 0x10040038u;
inline constexpr std::uint32_t GamercardTitleAchievementsEarned = 0x10040039u;
inline constexpr std::uint32_t GamerTier = 0x1004003Au;
inline constexpr std::uint32_t GamercardRep = 0x5004000Bu;
inline constexpr std::uint32_t GamercardPictureKey = 0x4064000Fu;
inline constexpr std::uint32_t GamercardMotto = 0x402C0011u;
inline constexpr std::uint32_t TitleSpecific1 = 0x63E83FFFu;
inline constexpr std::uint32_t TitleSpecific2 = 0x63E83FFEu;
inline constexpr std::uint32_t TitleSpecific3 = 0x63E83FFDu;
}  // namespace key

static_assert(key::GamerYAxisInversion == settingKey(DataType::Int32, 4, 0x02), "key layout");
static_assert(key::GamercardRep == settingKey(DataType::Float, 4, 0x0B), "key layout");
static_assert(key::GamercardPictureKey == settingKey(DataType::WString, 0x64, 0x0F), "key layout");
static_assert(key::GamercardMotto == settingKey(DataType::WString, 0x2C, 0x11), "key layout");
static_assert(key::TitleSpecific1 == settingKey(DataType::Binary, kMaxUserDataSize, 0x3FFF), "key layout");
static_assert(key::TitleSpecific3 == settingKey(DataType::Binary, kMaxUserDataSize, 0x3FFD), "key layout");

struct SettingDefinition {
    std::uint32_t key;
    const char* name;
    bool titleWritable;         // only TITLE_SPECIFIC1..3 (user_settings.h)
    std::uint64_t defaultBits;  // scalar union value (low 32 bits for 4-byte types)
    const char* wstringDefault; // ASCII default for wstring keys, else nullptr
};

// Defaults: zero-of-type unless the option enums / override table in
// user_settings.h say otherwise (vibration ON = 3; the dashboard picture key).
// GamerTier is 0 (no subscription) because this profile is not Live-enabled;
// Xenia reports Gold (6) for its profiles, which would misstate an offline one.
// Title-specific binaries default to "not set": source DEFAULT, 0 bytes.
inline constexpr std::array<SettingDefinition, 31> kSettingDefinitions = {{
    {key::GamerType, "XPROFILE_GAMER_TYPE", false, 0, nullptr},
    {key::GamerYAxisInversion, "XPROFILE_GAMER_YAXIS_INVERSION", false, 0, nullptr},
    {key::OptionControllerVibration, "XPROFILE_OPTION_CONTROLLER_VIBRATION", false, 3, nullptr},
    {key::GamercardZone, "XPROFILE_GAMERCARD_ZONE", false, 0, nullptr},
    {key::GamercardRegion, "XPROFILE_GAMERCARD_REGION", false, 0, nullptr},
    {key::GamercardCred, "XPROFILE_GAMERCARD_CRED", false, 0, nullptr},
    {key::OptionVoiceMuted, "XPROFILE_OPTION_VOICE_MUTED", false, 0, nullptr},
    {key::OptionVoiceThruSpeakers, "XPROFILE_OPTION_VOICE_THRU_SPEAKERS", false, 0, nullptr},
    {key::OptionVoiceVolume, "XPROFILE_OPTION_VOICE_VOLUME", false, 0, nullptr},
    {key::GamercardTitlesPlayed, "XPROFILE_GAMERCARD_TITLES_PLAYED", false, 0, nullptr},
    {key::GamercardAchievementsEarned, "XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED", false, 0, nullptr},
    {key::GamerDifficulty, "XPROFILE_GAMER_DIFFICULTY", false, 0, nullptr},
    {key::GamerControlSensitivity, "XPROFILE_GAMER_CONTROL_SENSITIVITY", false, 0, nullptr},
    {key::GamerPreferredColorFirst, "XPROFILE_GAMER_PREFERRED_COLOR_FIRST", false, 0, nullptr},
    {key::GamerPreferredColorSecond, "XPROFILE_GAMER_PREFERRED_COLOR_SECOND", false, 0, nullptr},
    {key::GamerActionAutoAim, "XPROFILE_GAMER_ACTION_AUTO_AIM", false, 0, nullptr},
    {key::GamerActionAutoCenter, "XPROFILE_GAMER_ACTION_AUTO_CENTER", false, 0, nullptr},
    {key::GamerActionMovementControl, "XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL", false, 0, nullptr},
    {key::GamerRaceTransmission, "XPROFILE_GAMER_RACE_TRANSMISSION", false, 0, nullptr},
    {key::GamerRaceCameraLocation, "XPROFILE_GAMER_RACE_CAMERA_LOCATION", false, 0, nullptr},
    {key::GamerRaceBrakeControl, "XPROFILE_GAMER_RACE_BRAKE_CONTROL", false, 0, nullptr},
    {key::GamerRaceAcceleratorControl, "XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL", false, 0, nullptr},
    {key::GamercardTitleCredEarned, "XPROFILE_GAMERCARD_TITLE_CRED_EARNED", false, 0, nullptr},
    {key::GamercardTitleAchievementsEarned, "XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED", false, 0, nullptr},
    {key::GamerTier, "XPROFILE_GAMER_TIER", false, 0, nullptr},
    {key::GamercardRep, "XPROFILE_GAMERCARD_REP", false, 0, nullptr},
    {key::GamercardPictureKey, "XPROFILE_GAMERCARD_PICTURE_KEY", false, 0, "FFFE07D10002000200010002"},
    {key::GamercardMotto, "XPROFILE_GAMERCARD_MOTTO", false, 0, ""},
    {key::TitleSpecific1, "XPROFILE_TITLE_SPECIFIC1", true, 0, nullptr},
    {key::TitleSpecific2, "XPROFILE_TITLE_SPECIFIC2", true, 0, nullptr},
    {key::TitleSpecific3, "XPROFILE_TITLE_SPECIFIC3", true, 0, nullptr},
}};

inline const SettingDefinition* findDefinition(std::uint32_t settingKeyValue) noexcept {
    for (const auto& definition : kSettingDefinitions)
        if (definition.key == settingKeyValue) return &definition;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Internal helpers.
// ---------------------------------------------------------------------------
namespace detail {

inline void storeBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
inline void storeBe64(std::uint8_t* p, std::uint64_t v) noexcept {
    storeBe32(p, static_cast<std::uint32_t>(v >> 32));
    storeBe32(p + 4, static_cast<std::uint32_t>(v));
}
inline std::uint32_t loadBe32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}
inline std::uint64_t loadBe64(const std::uint8_t* p) noexcept {
    return (std::uint64_t{loadBe32(p)} << 32) | loadBe32(p + 4);
}
inline void appendBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    std::uint8_t b[4];
    storeBe32(b, v);
    out.insert(out.end(), b, b + 4);
}

inline std::string errnoText(const char* what, int error) {
    return std::string(what) + " (errno " + std::to_string(error) + ")";
}
inline void setError(std::string* out, const std::string& text) {
    if (out) *out = text;
}

inline std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

inline std::string hex16(std::uint64_t v) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(v));
    return text;
}
inline std::string hex8(std::uint32_t v) {
    char text[9];
    std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(v));
    return text;
}

inline bool isDirectory(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p with 0700 for newly created components.
inline bool makeDirectories(const std::string& dir, std::string* error) {
    if (dir.empty()) {
        setError(error, "profile directory is empty");
        return false;
    }
    std::size_t pos = 0;
    while (pos <= dir.size()) {
        std::size_t next = dir.find('/', pos);
        if (next == std::string::npos) next = dir.size();
        const std::string partial = dir.substr(0, next);
        pos = next + 1;
        if (partial.empty() || isDirectory(partial)) continue;
        if (::mkdir(partial.c_str(), 0700) != 0) {
            const int e = errno;
            if (e == EEXIST && isDirectory(partial)) continue;
            setError(error, errnoText(("cannot create directory " + partial).c_str(), e));
            return false;
        }
    }
    if (!isDirectory(dir)) {
        setError(error, "profile path is not a directory: " + dir);
        return false;
    }
    return true;
}

enum class FileRead { Ok, Missing, Error };

inline FileRead readSmallFile(const std::string& path, std::size_t limit,
                              std::vector<std::uint8_t>& out, std::string* error) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        if (e == ENOENT) return FileRead::Missing;
        setError(error, errnoText(("cannot open " + path).c_str(), e));
        return FileRead::Error;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            static_cast<std::uint64_t>(st.st_size) > limit) {
        ::close(fd);
        setError(error, "not a bounded regular file: " + path);
        return FileRead::Error;
    }
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            const int e = n < 0 ? errno : 0;
            ::close(fd);
            setError(error, errnoText(("short read of " + path).c_str(), e));
            return FileRead::Error;
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return FileRead::Ok;
}

// temp file + fsync + rename + directory fsync (best effort on the directory).
inline bool writeFileAtomically(const std::string& dir, const std::string& path,
                                const std::vector<std::uint8_t>& bytes, std::string* error) {
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        setError(error, errnoText(("cannot create " + temp).c_str(), errno));
        return false;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            const int e = n < 0 ? errno : 0;
            ::close(fd);
            ::unlink(temp.c_str());
            setError(error, errnoText(("cannot write " + temp).c_str(), e));
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int e = errno;
        ::close(fd);
        ::unlink(temp.c_str());
        setError(error, errnoText(("cannot fsync " + temp).c_str(), e));
        return false;
    }
    if (::close(fd) != 0) {
        const int e = errno;
        ::unlink(temp.c_str());
        setError(error, errnoText(("cannot close " + temp).c_str(), e));
        return false;
    }
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        const int e = errno;
        ::unlink(temp.c_str());
        setError(error, errnoText(("cannot rename onto " + path).c_str(), e));
        return false;
    }
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

inline std::uint32_t fnv1a32(const std::uint8_t* data, std::size_t n) noexcept {
    std::uint32_t hash = 0x811C9DC5u;
    for (std::size_t i = 0; i < n; ++i) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

inline std::vector<std::uint8_t> asciiToUtf16BeWithNul(const char* text) {
    std::vector<std::uint8_t> out;
    for (const char* c = text; *c; ++c) {
        out.push_back(0);
        out.push_back(static_cast<std::uint8_t>(*c));
    }
    out.push_back(0);
    out.push_back(0);
    return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Identity.
// ---------------------------------------------------------------------------
struct Identity {
    std::uint64_t xuid = 0;  // 0 means "no usable identity" (failure)
    std::string gamertag;
};

inline constexpr std::uint64_t kOfflineXuidPrefix = 0xE000000000000000ull;  // [mutant:offline-prefix]
inline constexpr std::uint64_t kOfflineXuidRandomMask = 0x00000000FFFFFFFFull;
// Title 58410A1F refuses play when these bits are set (user_profile.cc note).
inline constexpr std::uint64_t kXuidRestrictedBits = 0x00C0000000000000ull;
inline constexpr const char* kDefaultGamertag = "Player";
inline constexpr const char* kIdentityFileName = "identity.txt";
inline constexpr const char* kIdentityHeader = "recomp-local-profile-identity v1";

inline bool isOfflineXuid(std::uint64_t xuid) noexcept { return (xuid >> 60) == 0xE; }

// Conservative ASCII subset of the gamertag rules: 1..15 chars, letters,
// digits and single inner spaces, starting with a letter. Fits char[16].
inline bool isValidGamertag(const std::string& tag) noexcept {
    if (tag.empty() || tag.size() > 15) return false;
    auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!letter(tag.front()) || tag.back() == ' ') return false;
    for (std::size_t i = 0; i < tag.size(); ++i) {
        const char c = tag[i];
        if (c == ' ' && i && tag[i - 1] == ' ') return false;
        if (!letter(c) && !digit(c) && c != ' ') return false;
    }
    return true;
}

inline bool isUsableIdentity(const Identity& identity) noexcept {
    return isOfflineXuid(identity.xuid) && !(identity.xuid & kXuidRestrictedBits) &&
           (identity.xuid & kOfflineXuidRandomMask) != 0 && isValidGamertag(identity.gamertag);
}

inline std::string identityPath(const std::string& dir) {
    return detail::joinPath(dir, kIdentityFileName);
}

inline std::vector<std::uint8_t> serializeIdentity(const Identity& identity) {
    const std::string text = std::string(kIdentityHeader) + "\nxuid=" + detail::hex16(identity.xuid) +
                             "\ngamertag=" + identity.gamertag + "\n";
    return {text.begin(), text.end()};
}

inline std::optional<Identity> parseIdentity(const std::vector<std::uint8_t>& bytes) {
    const std::string text(bytes.begin(), bytes.end());
    const std::string header = std::string(kIdentityHeader) + "\n";
    if (text.size() < header.size() || text.compare(0, header.size(), header) != 0) return std::nullopt;
    std::size_t pos = header.size();
    auto field = [&](const std::string& prefix, std::string& value) {
        if (text.size() - pos < prefix.size() || text.compare(pos, prefix.size(), prefix) != 0) return false;
        const std::size_t end = text.find('\n', pos + prefix.size());
        if (end == std::string::npos) return false;
        value = text.substr(pos + prefix.size(), end - pos - prefix.size());
        pos = end + 1;
        return true;
    };
    std::string xuidText, tag;
    if (!field("xuid=", xuidText) || !field("gamertag=", tag) || pos != text.size()) return std::nullopt;
    if (xuidText.size() != 16) return std::nullopt;
    std::uint64_t xuid = 0;
    for (const char c : xuidText) {
        std::uint64_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint64_t>(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint64_t>(c - 'a' + 10);
        else return std::nullopt;
        xuid = (xuid << 4) | digit;
    }
    Identity identity{xuid, tag};
    if (!isUsableIdentity(identity)) return std::nullopt;
    return identity;
}

// Loads <dir>/identity.txt, or creates it on first run with an offline XUID
// 0xE0000000'rrrrrrrr (r = low 32 bits of random64(), redrawn while zero)
// and gamertag "Player". A present-but-invalid file is never replaced (the
// XUID owns the saves); failures return xuid 0 and describe `error`.
inline Identity loadOrCreateIdentity(const std::string& dir, std::uint64_t (*random64)(),
                                     std::string* error = nullptr) noexcept {
    try {
        if (!detail::makeDirectories(dir, error)) return {};
        const std::string path = identityPath(dir);
        std::vector<std::uint8_t> bytes;
        switch (detail::readSmallFile(path, 4096, bytes, error)) {
            case detail::FileRead::Ok: {
                auto parsed = parseIdentity(bytes);
                if (!parsed) {
                    detail::setError(error, "identity file is invalid and was left untouched: " + path);
                    return {};
                }
                return *parsed;
            }
            case detail::FileRead::Error:
                return {};
            case detail::FileRead::Missing:
                break;
        }
        if (!random64) {
            detail::setError(error, "no random source for a new offline XUID");
            return {};
        }
        std::uint64_t low = 0;
        for (int attempt = 0; attempt < 8 && !low; ++attempt) low = random64() & kOfflineXuidRandomMask;
        if (!low) {
            detail::setError(error, "random source produced no nonzero XUID bits");
            return {};
        }
        Identity identity{kOfflineXuidPrefix | low, kDefaultGamertag};
        if (!isUsableIdentity(identity)) {
            detail::setError(error, "generated identity failed validation");
            return {};
        }
        if (!detail::writeFileAtomically(dir, path, serializeIdentity(identity), error)) return {};
        return identity;
    } catch (...) {
        detail::setError(error, "exception while loading identity");
        return {};
    }
}

// X_USER_SIGNIN_INFO as XamUserGetSigninInfo fills it for this profile.
inline std::array<std::uint8_t, 40> encodeSigninInfo(const Identity& identity, std::uint32_t signinState,
                                                     std::uint32_t queryFlags = 0) noexcept {
    std::array<std::uint8_t, 40> out{};
    if (!queryFlags || (queryFlags & kSigninInfoOfflineXuidOnly))
        detail::storeBe64(out.data() + 0, identity.xuid);
    detail::storeBe32(out.data() + 8, 0);             // info flags: not Live, not guest
    detail::storeBe32(out.data() + 12, signinState);  // [mutant:signin-state]
    detail::storeBe32(out.data() + 16, 0);            // guest number
    detail::storeBe32(out.data() + 20, 0);            // sponsor user index
    const std::size_t n = identity.gamertag.size() < 15 ? identity.gamertag.size() : 15;
    std::memcpy(out.data() + 24, identity.gamertag.data(), n);
    return out;
}

// Bytes XamUserGetName writes: min(bufferLen, len + 1) bytes, truncated and
// always NUL-terminated inside that count; empty when bufferLen is 0.
inline std::vector<std::uint8_t> encodeName(const Identity& identity, std::uint32_t bufferLen) {
    if (!bufferLen) return {};
    const std::size_t total = std::min<std::size_t>(bufferLen, identity.gamertag.size() + 1);
    std::vector<std::uint8_t> out(total, 0);
    std::memcpy(out.data(), identity.gamertag.data(), total - 1);
    return out;
}

// ---------------------------------------------------------------------------
// Profile settings.
// ---------------------------------------------------------------------------
struct SettingValue {
    std::uint32_t id = 0;
    Source source = Source::NoValue;
    DataType type = DataType::Null;
    // Scalars: 4 or 8 big-endian union bytes. WString: UTF-16BE including
    // the NUL terminator. Binary: raw bytes (empty = never written).
    std::vector<std::uint8_t> data;
};

struct DecodedSetting {
    std::uint32_t id = 0;
    std::vector<std::uint8_t> data;  // same representation as SettingValue::data
};

inline constexpr std::uint32_t kTitleFileMagic = 0x52585053u;  // "RXPS"
inline constexpr std::uint32_t kTitleFileVersion = 1;
inline constexpr std::size_t kTitleFileLimit = 64 * 1024;

inline std::string titleSettingsPath(const std::string& dir, std::uint32_t titleId) {
    return detail::joinPath(dir, "title_" + detail::hex8(titleId) + ".settings");
}

// File: magic, version, title id, count, {id, length, bytes}*, FNV-1a32 of
// everything before it. All integers big-endian.
inline std::vector<std::uint8_t> serializeTitleValues(
        std::uint32_t titleId, const std::map<std::uint32_t, std::vector<std::uint8_t>>& values) {
    std::vector<std::uint8_t> out;
    detail::appendBe32(out, kTitleFileMagic);
    detail::appendBe32(out, kTitleFileVersion);
    detail::appendBe32(out, titleId);
    detail::appendBe32(out, static_cast<std::uint32_t>(values.size()));
    for (const auto& [id, bytes] : values) {
        detail::appendBe32(out, id);
        detail::appendBe32(out, static_cast<std::uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    detail::appendBe32(out, detail::fnv1a32(out.data(), out.size()));
    return out;
}

inline bool parseTitleValues(const std::vector<std::uint8_t>& bytes, std::uint32_t titleId,
                             std::map<std::uint32_t, std::vector<std::uint8_t>>& values, std::string* error) {
    values.clear();
    auto fail = [&](const char* why) {
        values.clear();
        detail::setError(error, std::string("title settings file rejected: ") + why);
        return false;
    };
    if (bytes.size() < 20) return fail("too short");
    const std::size_t body = bytes.size() - 4;
    if (detail::fnv1a32(bytes.data(), body) != detail::loadBe32(bytes.data() + body)) return fail("checksum");
    if (detail::loadBe32(bytes.data()) != kTitleFileMagic) return fail("magic");
    if (detail::loadBe32(bytes.data() + 4) != kTitleFileVersion) return fail("version");
    if (detail::loadBe32(bytes.data() + 8) != titleId) return fail("title id");
    const std::uint32_t count = detail::loadBe32(bytes.data() + 12);
    std::size_t pos = 16;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (body - pos < 8) return fail("truncated record");
        const std::uint32_t id = detail::loadBe32(bytes.data() + pos);
        const std::uint32_t length = detail::loadBe32(bytes.data() + pos + 4);
        pos += 8;
        const auto* definition = findDefinition(id);
        if (!definition || !definition->titleWritable) return fail("record is not a title setting");
        if (length > maxDataSize(id) || body - pos < length) return fail("record length");
        if (values.count(id)) return fail("duplicate record");
        values[id].assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                          bytes.begin() + static_cast<std::ptrdiff_t>(pos + length));
        pos += length;
    }
    if (pos != body) return fail("trailing bytes");
    return true;
}

// System settings: read-only defaults. XAM only lets titles write
// TITLE_SPECIFIC1..3 (user_settings.h title_writable_settings); a title write
// of any other known setting returns kErrorAccessDenied (local policy code).
// Title settings: persisted per title id in <dir>/title_XXXXXXXX.settings.
// A corrupt or unreadable title file makes ok() false: title-setting reads
// fail and writes refuse, so a damaged save-settings file is never replaced.
// Not thread-safe; one instance per (profile dir, title id).
class Settings {
public:
    explicit Settings(std::string dir, std::uint32_t titleId) noexcept : dir_(std::move(dir)), titleId_(titleId) {
        try {
            load();
        } catch (...) {
            ok_ = false;
            titleValues_.clear();
            error_ = "exception while loading title settings";
        }
    }

    bool ok() const noexcept { return ok_; }
    const std::string& error() const noexcept { return error_; }
    std::uint32_t titleId() const noexcept { return titleId_; }
    std::string storagePath() const { return titleSettingsPath(dir_, titleId_); }  // [mutant:title-path]

    std::optional<SettingValue> read(std::uint32_t settingId) const noexcept {
        try {
            const auto* definition = findDefinition(settingId);
            if (!definition) return std::nullopt;
            SettingValue value;
            value.id = settingId;
            value.type = static_cast<DataType>(keyType(settingId));
            value.source = Source::Default;
            if (definition->titleWritable) {
                if (!ok_) return std::nullopt;
                const auto found = titleValues_.find(settingId);
                if (found != titleValues_.end()) {
                    value.source = Source::Title;
                    value.data = found->second;
                }
                return value;
            }
            if (definition->wstringDefault) {
                value.data = detail::asciiToUtf16BeWithNul(definition->wstringDefault);
            } else if (const auto width = scalarWidth(keyType(settingId))) {
                value.data.resize(width);
                if (width == 4) detail::storeBe32(value.data.data(), static_cast<std::uint32_t>(definition->defaultBits));
                else detail::storeBe64(value.data.data(), definition->defaultBits);
            }
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::uint32_t write(std::uint32_t settingId, const std::uint8_t* data, std::size_t n) noexcept {
        try {
            if (n && !data) return kErrorInvalidParameter;
            DecodedSetting setting;
            setting.id = settingId;
            if (n) setting.data.assign(data, data + n);
            return writeAll({setting});
        } catch (...) {
            error_ = "exception while writing title settings";
            return kErrorWriteFault;
        }
    }

    // Validates every entry first; persists all or nothing in one file replace.
    std::uint32_t writeAll(const std::vector<DecodedSetting>& settings) noexcept {
        try {
            for (const auto& setting : settings) {
                const auto* definition = findDefinition(setting.id);
                if (!definition) return kErrorInvalidParameter;
                if (!definition->titleWritable) return kErrorAccessDenied;
                if (setting.data.size() > maxDataSize(setting.id)) return kErrorInvalidParameter;
            }
            if (settings.empty()) return kErrorSuccess;
            if (!ok_) return kErrorStorageFault;
            auto next = titleValues_;
            for (const auto& setting : settings) next[setting.id] = setting.data;
            if (!detail::makeDirectories(dir_, &error_)) return kErrorWriteFault;
            if (!detail::writeFileAtomically(dir_, storagePath(), serializeTitleValues(titleId_, next), &error_))
                return kErrorWriteFault;
            titleValues_ = std::move(next);
            return kErrorSuccess;
        } catch (...) {
            error_ = "exception while writing title settings";
            return kErrorWriteFault;
        }
    }

private:
    void load() {
        ok_ = false;
        if (!titleId_) {
            error_ = "title id 0 is unresolved; pass the running title id";
            return;
        }
        std::vector<std::uint8_t> bytes;
        switch (detail::readSmallFile(storagePath(), kTitleFileLimit, bytes, &error_)) {
            case detail::FileRead::Missing:
                ok_ = true;
                return;
            case detail::FileRead::Error:
                return;
            case detail::FileRead::Ok:
                ok_ = parseTitleValues(bytes, titleId_, titleValues_, &error_);
                return;
        }
    }

    std::string dir_;
    std::uint32_t titleId_;
    bool ok_ = false;
    std::string error_;
    std::map<std::uint32_t, std::vector<std::uint8_t>> titleValues_;
};

// ---------------------------------------------------------------------------
// XamUserReadProfileSettings buffer encoding.
// ---------------------------------------------------------------------------
struct ReadResult {
    std::uint32_t status = kErrorInvalidParameter;  // 0, 87, 122 or kErrorStorageFault
    std::uint32_t needed = 0;                       // valid for 0 and 122
    bool writeNeededSize = false;  // 122 path: write `needed` to *size only if caller's size was 0
    bool storageFault = false;     // registry should reject, not forward the status
    std::uint32_t settingCount = 0;               // header count after compaction
    std::vector<std::uint8_t> bytes;              // exactly `needed` bytes for guestBufferAddress
    std::vector<std::uint32_t> skippedIds;        // requested ids not modeled (compacted out)
};

// Byte-for-byte the registry's `needed` computation in the
// __imp__XamUserReadProfileSettings handler (and Xenia without xuids):
// 8-byte header + 40 per id + the key's 12-bit size for wstring/binary keys.
// Callers bound ids.size() to 1..32 first.
inline std::uint32_t requiredReadSize(const std::vector<std::uint32_t>& ids) noexcept {
    std::uint32_t needed = kReadHeaderSize + kSettingRecordSize * static_cast<std::uint32_t>(ids.size());
    for (const auto key : ids)
        if ((key >> 28) == 4 || (key >> 28) == 6) needed += (key >> 16) & 0xFFF;  // [mutant:needed]
    return needed;
}

// Mirrors the registry/Xenia order: count 1..32, nonzero size needs a buffer,
// size check (size written back only when the caller passed 0), then the
// profile lookup. Unknown ids are skipped and the header count decremented,
// so later records shift down (xam_user.cc); trailing slots stay zero.
inline ReadResult encodeRead(std::uint32_t guestBufferAddress, std::uint32_t bufferSize, std::uint32_t userIndex,
                             const std::vector<std::uint32_t>& ids, const Settings& settings) noexcept {
    ReadResult result;
    try {
        if (ids.empty() || ids.size() > kMaxReadSettings) return result;
        if (bufferSize && !guestBufferAddress) return result;
        result.needed = requiredReadSize(ids);
        if (!guestBufferAddress || bufferSize < result.needed) {
            result.status = kErrorInsufficientBuffer;
            result.writeNeededSize = bufferSize == 0;
            return result;
        }
        if (std::uint64_t{guestBufferAddress} + result.needed > 0x100000000ull) {
            result.status = kErrorInvalidParameter;
            return result;
        }
        auto storageFault = [&] {
            ReadResult fault;
            fault.status = kErrorStorageFault;
            fault.needed = result.needed;
            fault.storageFault = true;
            return fault;
        };
        if (!settings.ok()) return storageFault();

        result.bytes.assign(result.needed, 0);
        std::uint8_t* out = result.bytes.data();
        const std::uint32_t count = static_cast<std::uint32_t>(ids.size());
        const std::uint32_t settingsPointer = guestBufferAddress + kReadHeaderSize;
        std::uint32_t cursor = kReadHeaderSize + kSettingRecordSize * count;  // offset in `bytes`
        std::uint32_t written = 0;
        for (const auto id : ids) {
            const auto value = settings.read(id);
            if (!value) {
                result.skippedIds.push_back(id);
                continue;
            }
            std::uint8_t* record = out + kReadHeaderSize + kSettingRecordSize * written;
            detail::storeBe32(record + 0, static_cast<std::uint32_t>(value->source));
            detail::storeBe32(record + 8, userIndex);    // user_index over xuid = -1
            detail::storeBe32(record + 12, 0xFFFFFFFFu); // [mutant:xuid-low]
            detail::storeBe32(record + 16, id);
            record[24] = static_cast<std::uint8_t>(value->type);
            if (keyHasVariableData(id)) {
                if (!value->data.empty()) {
                    const std::uint32_t size = static_cast<std::uint32_t>(value->data.size());
                    if (size > maxDataSize(id) || cursor + size > result.needed) return storageFault();
                    std::memcpy(out + cursor, value->data.data(), size);
                    detail::storeBe32(record + 32, size);
                    detail::storeBe32(record + 36, guestBufferAddress + cursor);
                    cursor += maxDataSize(id);  // [mutant:cursor]
                }
            } else if (value->data.size() == 4 || value->data.size() == 8) {
                std::memcpy(record + 32, value->data.data(), value->data.size());
            } else {
                return storageFault();
            }
            ++written;
        }
        result.settingCount = written;
        detail::storeBe32(out + 0, written);
        detail::storeBe32(out + 4, settingsPointer);  // [mutant:settings-ptr]
        result.status = kErrorSuccess;
        return result;
    } catch (...) {
        ReadResult fault;
        fault.status = kErrorStorageFault;
        fault.storageFault = true;
        return fault;
    }
}

// ---------------------------------------------------------------------------
// XamUserWriteProfileSettings decoding.
// ---------------------------------------------------------------------------
// Copies `size` guest bytes at `address` into `out`; false if not readable.
using GuestReader = std::function<bool(std::uint32_t address, std::uint32_t size, std::uint8_t* out)>;

struct DecodeWrite {
    std::uint32_t status = kErrorInvalidParameter;  // 0 or 87
    bool memoryFault = false;  // registry should raise its memory-fault path
    std::uint32_t failedIndex = kNoIndex;
    std::string error;
    std::vector<DecodedSetting> settings;
};

// Structural validation of the guest X_USER_PROFILE_SETTING array. Stricter
// than Xenia (which re-derives the type from the key and skips bad records):
// any bad record fails the whole call so nothing is partially written.
// Policy (title-writable, known id) is Settings::writeAll's job.
inline DecodeWrite decodeWrite(const GuestReader& read, std::uint32_t count, std::uint32_t settingsAddress) noexcept {
    DecodeWrite result;
    try {
        auto fail = [&](std::uint32_t index, const char* why, bool fault = false) {
            DecodeWrite failed;
            failed.status = kErrorInvalidParameter;
            failed.memoryFault = fault;
            failed.failedIndex = index;
            failed.error = why;
            return failed;
        };
        if (!count || !settingsAddress) return fail(kNoIndex, "null or empty setting array");
        if (count > kMaxWriteSettings) return fail(kNoIndex, "setting count above local bound");
        if (!read) return fail(kNoIndex, "no guest reader", true);
        const std::uint64_t total = std::uint64_t{count} * kSettingRecordSize;
        if (std::uint64_t{settingsAddress} + total > 0x100000000ull)
            return fail(kNoIndex, "setting array wraps the guest address space", true);
        std::vector<std::uint8_t> records(static_cast<std::size_t>(total));
        if (!read(settingsAddress, static_cast<std::uint32_t>(total), records.data()))
            return fail(kNoIndex, "setting array is not readable guest memory", true);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* record = records.data() + std::size_t{i} * kSettingRecordSize;
            const std::uint32_t id = detail::loadBe32(record + 16);
            const std::uint32_t type = keyType(id);
            if (type > static_cast<std::uint32_t>(DataType::DateTime)) return fail(i, "invalid key type");
            if (record[24] != type) return fail(i, "data type does not match the key type");
            DecodedSetting setting;
            setting.id = id;
            if (keyHasVariableData(id)) {
                const std::uint32_t size = detail::loadBe32(record + 32);
                const std::uint32_t pointer = detail::loadBe32(record + 36);
                if (size > maxDataSize(id)) return fail(i, "value exceeds the key size");
                if (type == static_cast<std::uint32_t>(DataType::WString) && (size & 1))
                    return fail(i, "odd UTF-16 byte count");
                if (size) {
                    if (!pointer) return fail(i, "null value pointer");
                    if (std::uint64_t{pointer} + size > 0x100000000ull)
                        return fail(i, "value wraps the guest address space", true);
                    setting.data.resize(size);
                    if (!read(pointer, size, setting.data.data()))
                        return fail(i, "value is not readable guest memory", true);
                    if (type == static_cast<std::uint32_t>(DataType::WString) &&
                            (setting.data[size - 2] || setting.data[size - 1]))
                        return fail(i, "wstring is not NUL-terminated");
                }
            } else {
                const std::uint32_t width = scalarWidth(type);
                if (keySize(id) != width) return fail(i, "scalar key size does not match its type");
                setting.data.assign(record + 32, record + 32 + width);
            }
            result.settings.push_back(std::move(setting));
        }
        result.status = kErrorSuccess;
        return result;
    } catch (...) {
        DecodeWrite failed;
        failed.status = kErrorInvalidParameter;
        failed.memoryFault = true;
        failed.error = "exception while decoding";
        return failed;
    }
}

}  // namespace recomp::bootstrap::profile
