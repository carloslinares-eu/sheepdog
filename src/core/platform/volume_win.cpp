// Windows volume information: GetVolumePathNameW, GetVolumeInformationW, the 64-bit volume
// serial from FILE_ID_INFO and the seek-penalty query for HDD versus SSD.

#include "core/platform/fs.hpp"
#include "core/platform/win_util.hpp"

#include <array>
#include <vector>
#include <winioctl.h>

namespace sheepdog::platform {

namespace {

std::string utf8_of(const wchar_t* wide) {
    const std::wstring_view view(wide);
    return display_string(wtf8_from_utf16(
        std::u16string_view(reinterpret_cast<const char16_t*>(view.data()), view.size())));
}

// "C:\" or "\\server\share\" from "\\?\C:\" or "\\?\UNC\server\share\", for the older APIs.
std::wstring plain_root(const std::wstring& root) {
    if (root.rfind(LR"(\\?\UNC\)", 0) == 0) {
        return L"\\\\" + root.substr(8);
    }
    if (root.rfind(LR"(\\?\)", 0) == 0) {
        return root.substr(4);
    }
    return root;
}

// HDD or SSD from IOCTL_STORAGE_QUERY_PROPERTY(StorageDeviceSeekPenaltyProperty). Needs no
// administrator rights because the volume is opened with no access rights at all.
Media media_of(const std::wstring& root) {
    std::array<wchar_t, 64> volume{};
    if (::GetVolumeNameForVolumeMountPointW(root.c_str(), volume.data(),
                                            static_cast<DWORD>(volume.size())) == 0) {
        return Media::unknown;
    }
    std::wstring device(volume.data());
    if (!device.empty() && device.back() == L'\\') {
        device.pop_back(); // "\\?\Volume{...}" names the volume device itself
    }
    const win::Handle h(::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h.valid()) {
        return Media::unknown;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
    DWORD returned = 0;
    if (::DeviceIoControl(h.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &penalty,
                          sizeof penalty, &returned, nullptr) == 0 ||
        returned < sizeof penalty) {
        return Media::unknown;
    }
    return penalty.IncursSeekPenalty != FALSE ? Media::hdd : Media::ssd;
}

} // namespace

Result<VolumeInfo> volume_info(const NativePath& path) {
    auto wide = win32_path(path);
    if (!wide) {
        return wide.error();
    }
    std::vector<wchar_t> root_buf(wide->size() + 2, L'\0');
    if (::GetVolumePathNameW(wide->c_str(), root_buf.data(), static_cast<DWORD>(root_buf.size())) ==
        0) {
        return last_error(path);
    }
    const std::wstring root(root_buf.data());

    std::array<wchar_t, MAX_PATH + 1> label{};
    std::array<wchar_t, MAX_PATH + 1> fs_name{};
    DWORD serial32 = 0;
    DWORD max_component = 0;
    DWORD flags = 0;
    if (::GetVolumeInformationW(root.c_str(), label.data(), static_cast<DWORD>(label.size()),
                                &serial32, &max_component, &flags, fs_name.data(),
                                static_cast<DWORD>(fs_name.size())) == 0) {
        return last_error(path);
    }

    VolumeInfo info;
    info.label = utf8_of(label.data());
    info.fs_type = utf8_of(fs_name.data());
    info.mount_root = native_from_wide(root);

    // The same 64-bit serial that FILE_ID_INFO reports for every file on the volume.
    unsigned long long serial = serial32;
    const win::Handle h = win::open_for_attributes(root);
    FileId root_id;
    if (h.valid() && win::read_file_id(h.get(), root_id)) {
        serial = root_id.volume;
    }
    info.uid = "win:" + win::hex64(serial);

    info.remote = ::GetDriveTypeW(plain_root(root).c_str()) == DRIVE_REMOTE;
    info.media = info.remote ? Media::unknown : media_of(root);
    // NTFS reports FILE_CASE_SENSITIVE_SEARCH although Win32 lookups ignore case, so probe.
    info.case_sensitive = probe_case_sensitive(path).value_or(false);
    return info;
}

} // namespace sheepdog::platform
