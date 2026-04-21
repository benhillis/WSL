/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    registry.cpp

Abstract:

    This file contains registry management helper function implementation.

--*/

#include "precomp.h"
#include "registry.hpp"
#include "svccomm.hpp"
#pragma hdrstop

namespace {
std::wstring GetKeyPath(_In_ HKEY Key)
{
    if (Key == HKEY_LOCAL_MACHINE)
    {
        return L"HKLM";
    }
    else if (Key == HKEY_CLASSES_ROOT)
    {
        return L"HKCR";
    }
    else if (Key == HKEY_USERS)
    {
        return L"HKU";
    }
    else if (Key == HKEY_CURRENT_USER)
    {
        return L"HKCU";
    }
    else if (Key == HKEY_CURRENT_CONFIG)
    {
        return L"HKCC";
    }

    ULONG requiredSize{};
    auto status = ZwQueryKey(Key, KeyNameInformation, nullptr, 0, &requiredSize);
    if (status != STATUS_BUFFER_TOO_SMALL)
    {
        THROW_NTSTATUS(status);
    }

    std::vector<char> buffer(requiredSize, 0);

    status = ZwQueryKey(Key, KeyNameInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &requiredSize);
    THROW_IF_WIN32_ERROR(status);

    const auto* info = reinterpret_cast<KEY_NAME_INFORMATION*>(buffer.data());

    return std::wstring{info->Name, info->NameLength / sizeof(WCHAR)};
}

void ReportHrErrorIfFailed(_In_ HRESULT hr, _In_ HKEY Key, _In_opt_ LPCWSTR Subkey, _In_opt_ LPCWSTR Value)
{
    if (SUCCEEDED(hr))
    {
        return;
    }

    if (Key == nullptr)
    {
        const auto errorString = wsl::windows::common::wslutil::GetSystemErrorString(hr);
        THROW_HR_WITH_USER_ERROR(
            hr, wsl::shared::Localization::MessageRegistryError(Subkey ? Subkey : L"[null]", errorString.c_str()).c_str());
    }

    auto path = GetKeyPath(Key);
    if (Subkey != nullptr)
    {
        path += L"\\" + std::wstring(Subkey);
    }

    if (Value != nullptr)
    {
        path += L"\\" + std::wstring(Value);
    }

    if (wsl::windows::common::ExecutionContext::ShouldCollectErrorMessage())
    {
        const auto errorString = wsl::windows::common::wslutil::GetSystemErrorString(hr);
        THROW_HR_WITH_USER_ERROR(hr, wsl::shared::Localization::MessageRegistryError(path.c_str(), errorString.c_str()).c_str());
    }
    else
    {
        THROW_HR_MSG(hr, "An error occurred accessing the registry. Path: %ls", path.c_str());
    }
}

void ReportErrorIfFailed(_In_ LSTATUS Error, _In_ HKEY Key, _In_opt_ LPCWSTR Subkey, _In_opt_ LPCWSTR Value)
{
    if (Error == ERROR_SUCCESS)
    {
        return;
    }

    ReportHrErrorIfFailed(HRESULT_FROM_WIN32(Error), Key, Subkey, Value);
}
} // namespace
void wsl::windows::common::registry::ClearSubkeys(_In_ HKEY Key)
{
    for (const auto& e : EnumKeys(Key, KEY_READ))
    {
        DeleteKey(Key, e.first.c_str());
    }
}

wil::unique_hkey wsl::windows::common::registry::CreateKey(
    _In_ HKEY Key, _In_ LPCWSTR KeyName, _In_ REGSAM AccessMask, _Out_opt_ LPDWORD Disposition, _In_ DWORD Options)
{
    wil::unique_hkey NewKey;
    THROW_IF_WIN32_ERROR(RegCreateKeyExW(Key, KeyName, 0, nullptr, Options, AccessMask, nullptr, &NewKey, Disposition));

    return NewKey;
}

bool wsl::windows::common::registry::DeleteKey(_In_ HKEY Key, _In_ LPCWSTR KeyName)
{
    const LSTATUS Result = RegDeleteTreeW(Key, KeyName);
    if (Result != ERROR_FILE_NOT_FOUND)
    {
        LOG_IF_WIN32_ERROR(Result);
    }

    return Result == NO_ERROR;
}

void wsl::windows::common::registry::DeleteKeyValue(_In_ HKEY Key, _In_ LPCWSTR KeyName)
{
    const LSTATUS Result = RegDeleteKeyValueW(Key, nullptr, KeyName);
    if (Result != ERROR_FILE_NOT_FOUND)
    {
        LOG_IF_WIN32_ERROR(Result);
    }
}

void wsl::windows::common::registry::DeleteValue(_In_ HKEY Key, _In_ LPCWSTR KeyName)
{
    const LSTATUS Result = RegDeleteValueW(Key, KeyName);
    if (Result != ERROR_FILE_NOT_FOUND)
    {
        LOG_IF_WIN32_ERROR(Result);
    }
}

std::map<std::wstring, wil::unique_hkey> wsl::windows::common::registry::EnumKeys(_In_ HKEY Key, _In_ DWORD SubkeyAccess)
{
    std::map<std::wstring, wil::unique_hkey> keys;
    for (const auto& keyData : wil::make_range(wil::reg::key_iterator{Key}, wil::reg::key_iterator{}))
    {
        auto subKey = OpenKey(Key, keyData.name.c_str(), SubkeyAccess);
        keys.emplace(keyData.name, std::move(subKey));
    }

    return keys;
}

std::vector<std::pair<GUID, std::wstring>> wsl::windows::common::registry::EnumGuidKeys(_In_ HKEY Key)
{
    std::vector<std::pair<GUID, std::wstring>> subKeys;
    for (const auto& keyData : wil::make_range(wil::reg::key_iterator{Key}, wil::reg::key_iterator{}))
    {
        // Ignore any subkeys that are not GUIDs.
        auto guid = wsl::shared::string::ToGuid(keyData.name.c_str());
        if (!guid.has_value())
        {
            continue;
        }

        subKeys.emplace_back(std::make_pair(guid.value(), keyData.name));
    }

    return subKeys;
}

std::vector<std::pair<std::wstring, DWORD>> wsl::windows::common::registry::EnumValues(_In_ HKEY Key)
{
    std::vector<std::pair<std::wstring, DWORD>> values;
    for (const auto& valueData : wil::make_range(wil::reg::value_iterator{Key}, wil::reg::value_iterator{}))
    {
        values.emplace_back(valueData.name, valueData.type);
    }

    return values;
}

bool wsl::windows::common::registry::IsKeyVolatile(_In_ HKEY Key)
{
    KEY_FLAGS_INFORMATION info{};
    DWORD resultSize{};
    THROW_IF_NTSTATUS_FAILED(ZwQueryKey(Key, KeyFlagsInformation, &info, sizeof(info), &resultSize));

    return WI_IsFlagSet(info.KeyFlags, REG_OPTION_VOLATILE);
}

wil::unique_hkey wsl::windows::common::registry::OpenCurrentUser(_In_ REGSAM AccessMask)
{
    wil::unique_hkey UserKey;
    THROW_IF_WIN32_ERROR(RegOpenCurrentUser(AccessMask, &UserKey));

    return UserKey;
}

std::pair<wil::unique_hkey, HRESULT> wsl::windows::common::registry::OpenKeyNoThrow(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ REGSAM AccessMask, _In_ DWORD Options)
{
    wil::unique_hkey OpenedKey;
    const auto error = RegOpenKeyExW(Key, SubKey, Options, AccessMask, &OpenedKey);

    return {std::move(OpenedKey), HRESULT_FROM_WIN32(error)};
}

wil::unique_hkey wsl::windows::common::registry::OpenKey(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ REGSAM AccessMask, _In_ DWORD Options)
{
    auto [key, error] = OpenKeyNoThrow(Key, SubKey, AccessMask, Options);
    ReportHrErrorIfFailed(error, Key, SubKey, nullptr);

    return std::move(key);
}

wil::unique_hkey wsl::windows::common::registry::OpenLxssMachineKey(REGSAM AccessMask)
{
    wil::unique_hkey LxssKey = CreateKey(HKEY_LOCAL_MACHINE, LXSS_REGISTRY_PATH, AccessMask);
    THROW_LAST_ERROR_IF(!LxssKey);

    return LxssKey;
}

wil::unique_hkey wsl::windows::common::registry::OpenLxssUserKey()
{
    const wil::unique_hkey UserKey = OpenCurrentUser();
    wil::unique_hkey LxssKey = CreateKey(UserKey.get(), LXSS_REGISTRY_PATH);
    THROW_LAST_ERROR_IF(!LxssKey);

    return LxssKey;
}

wil::unique_hkey wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(_In_ PSID UserSid)
{
    // In this method we use the user SID to open a user specific key under HKLM
    // The reason for not using HKCU is that lxss trusts this key and will mount
    // all the volumes listed under it.
    // Given that only elevated users are allowed to mount disks, using HKCU would
    // create a security issue as non-admin users could write anything they want there.
    std::wstring path = std::format(L"{}\\{}", LXSS_DISK_MOUNTS_REGISTRY_PATH, wsl::windows::common::wslutil::SidToString(UserSid).get());

    // Create a volatile key so that disk states aren't kept after a reboot
    return CreateKey(HKEY_LOCAL_MACHINE, path.c_str(), KEY_ALL_ACCESS, nullptr, REG_OPTION_VOLATILE);
}

DWORD
wsl::windows::common::registry::ReadDword(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_ DWORD DefaultValue)
{
    DWORD value{};
    const auto hr = wil::reg::get_value_dword_nothrow(Key, KeyName, ValueName, &value);
    if (hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
    {
        return DefaultValue;
    }

    ReportHrErrorIfFailed(hr, Key, KeyName, ValueName);
    return value;
}

ULONG64
wsl::windows::common::registry::ReadQword(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_ ULONG64 DefaultValue)
{
    uint64_t value{};
    const auto hr = wil::reg::get_value_qword_nothrow(Key, KeyName, ValueName, &value);
    if (hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
    {
        return DefaultValue;
    }

    ReportHrErrorIfFailed(hr, Key, KeyName, ValueName);
    return value;
}

std::wstring wsl::windows::common::registry::ReadString(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_opt_ LPCWSTR Default)
{
    auto value = ReadOptionalString(Key, KeyName, ValueName);
    if (!value.has_value())
    {
        if (ARGUMENT_PRESENT(Default))
        {
            return Default;
        }
        else
        {
            ReportErrorIfFailed(ERROR_PATH_NOT_FOUND, Key, KeyName, ValueName);
        }
    }

    return value.value();
}

std::optional<std::wstring> wsl::windows::common::registry::ReadOptionalString(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName)
{
    return wil::reg::try_get_value_string(Key, KeyName, ValueName);
}

std::vector<std::string> wsl::windows::common::registry::ReadStringSet(
    _In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, const std::vector<std::string>& Default)
{
    auto wideStrings = wil::reg::try_get_value_multistring(Key, KeyName, ValueName);
    if (!wideStrings.has_value())
    {
        return Default;
    }

    std::vector<std::string> values;
    for (const auto& ws : wideStrings.value())
    {
        values.push_back(wsl::shared::string::WideToMultiByte(ws.c_str()));
    }

    return values;
}

void wsl::windows::common::registry::WriteDword(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR ValueName, _In_ DWORD Value)
{
    const auto hr = wil::reg::set_value_dword_nothrow(Key, SubKey, ValueName, Value);
    ReportHrErrorIfFailed(hr, Key, SubKey, ValueName);
}

void wsl::windows::common::registry::WriteQword(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR ValueName, _In_ ULONG64 Value)
{
    const auto hr = wil::reg::set_value_qword_nothrow(Key, SubKey, ValueName, Value);
    ReportHrErrorIfFailed(hr, Key, SubKey, ValueName);
}

void wsl::windows::common::registry::WriteDefaultString(_In_ HKEY Key, _In_ LPCWSTR Value)
{
    const auto hr = wil::reg::set_value_string_nothrow(Key, nullptr, Value);
    ReportHrErrorIfFailed(hr, Key, nullptr, nullptr);
}

void wsl::windows::common::registry::WriteString(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR ValueName, _In_ LPCWSTR Value)
{
    const auto hr = wil::reg::set_value_string_nothrow(Key, SubKey, ValueName, Value);
    ReportHrErrorIfFailed(hr, Key, SubKey, ValueName);
}

void wsl::windows::common::registry::WriteStringSet(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR ValueName, _In_ const std::vector<std::wstring>& StringSet)
{
    THROW_HR_IF(E_INVALIDARG, (StringSet.size() == 0));

    // Build the double-null-terminated multi-string buffer manually since WIL lacks a nothrow multistring set API.
    std::vector<WCHAR> buffer;
    for (const auto& s : StringSet)
    {
        buffer.insert(buffer.end(), s.begin(), s.end());
        buffer.push_back(UNICODE_NULL);
    }

    buffer.push_back(UNICODE_NULL);

    const auto size = buffer.size() * sizeof(WCHAR);
    THROW_HR_IF(E_INVALIDARG, (size > static_cast<size_t>(DWORD_MAX)));

    const auto result = RegSetKeyValueW(Key, SubKey, ValueName, REG_MULTI_SZ, buffer.data(), static_cast<DWORD>(size));
    ReportErrorIfFailed(result, Key, SubKey, ValueName);
}