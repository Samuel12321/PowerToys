#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <variant>
#include <vector>
#include <optional>
#include <cassert>
#include <sstream>

#include "../logger/logger.h"
#include "../utils/winapi_error.h"
#include "../version/version.h"

namespace registry
{
    namespace detail
    {
        struct on_exit
        {
            std::function<void()> f;

            on_exit(std::function<void()> f) :
                f{ std::move(f) } {}
            ~on_exit() { f(); }
        };

        template<class>
        inline constexpr bool always_false_v = false;

        template<class... Ts>
        struct overloaded : Ts...
        {
            using Ts::operator()...;
        };

        template<class... Ts>
        overloaded(Ts...) -> overloaded<Ts...>;

        inline const wchar_t* getScopeName(HKEY scope)
        {
            if (scope == HKEY_LOCAL_MACHINE)
            {
                return L"HKLM";
            }
            else if (scope == HKEY_CURRENT_USER)
            {
                return L"HKCU";
            }
            else if (scope == HKEY_CLASSES_ROOT)
            {
                return L"HKCR";
            }
            else
            {
                return L"HK??";
            }
        }
    }

    struct ValueChange
    {
        using value_t = std::variant<DWORD, std::wstring>;
        static constexpr size_t VALUE_BUFFER_SIZE = 512;

        HKEY scope{};
        std::wstring path;
        std::optional<std::wstring> name; // none == default
        value_t value;

        ValueChange(const HKEY scope, std::wstring path, std::optional<std::wstring> name, value_t value) :
            scope{ scope }, path{ std::move(path) }, name{ std::move(name) }, value{ std::move(value) }
        {
        }

        std::wstring toString() const
        {
            using namespace detail;

            std::wstring value_str;
            std::visit(overloaded{ [&](DWORD value) {
                                      std::wostringstream oss;
                                      oss << value;
                                      value_str = oss.str();
                                  },
                                   [&](const std::wstring& value) { value_str = value; } },
                       value);

            return fmt::format(L"{}\\{}\\{}:{}", detail::getScopeName(scope), path, name ? *name : L"Default", value_str);
        }

        bool isApplied() const
        {
            HKEY key{};
            if (auto res = RegOpenKeyExW(scope, path.c_str(), 0, KEY_READ, &key); res != ERROR_SUCCESS)
            {
                Logger::info(L"isApplied of {}: RegOpenKeyExW failed: {}", toString(), get_last_error_or_default(res));
                return false;
            }
            detail::on_exit closeKey{ [key] { RegCloseKey(key); } };

            const DWORD expectedType = valueTypeToWinapiType(value);

            DWORD retrievedType{};
            wchar_t buffer[VALUE_BUFFER_SIZE];
            DWORD valueSize = sizeof(buffer);
            if (auto res = RegQueryValueExW(key,
                                            name.has_value() ? name->c_str() : nullptr,
                                            0,
                                            &retrievedType,
                                            reinterpret_cast<LPBYTE>(&buffer),
                                            &valueSize);
                res != ERROR_SUCCESS)
            {
                Logger::info(L"isApplied of {}: RegQueryValueExW failed: {}", toString(), get_last_error_or_default(res));
                return false;
            }

            if (expectedType != retrievedType)
            {
                return false;
            }

            if (const auto retrievedValue = bufferToValue(buffer, valueSize, retrievedType))
            {
                return value == retrievedValue;
            }
            else
            {
                return false;
            }
        }

        bool apply() const
        {
            HKEY key{};

            if (auto res = RegCreateKeyExW(scope, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr); res !=
                                                                                                                                         ERROR_SUCCESS)
            {
                Logger::error(L"apply of {}: RegCreateKeyExW failed: {}", toString(), get_last_error_or_default(res));
                return false;
            }
            detail::on_exit closeKey{ [key] { RegCloseKey(key); } };

            wchar_t buffer[VALUE_BUFFER_SIZE];
            DWORD valueSize;
            DWORD valueType;

            valueToBuffer(value, buffer, valueSize, valueType);
            if (auto res = RegSetValueExW(key,
                                          name.has_value() ? name->c_str() : nullptr,
                                          0,
                                          valueType,
                                          reinterpret_cast<BYTE*>(buffer),
                                          valueSize);
                res != ERROR_SUCCESS)
            {
                Logger::error(L"apply of {}: RegSetValueExW failed: {}", toString(), get_last_error_or_default(res));
                return false;
            }

            return true;
        }

        bool unApply() const
        {
            HKEY key{};
            if (auto res = RegOpenKeyExW(scope, path.c_str(), 0, KEY_ALL_ACCESS, &key); res != ERROR_SUCCESS)
            {
                Logger::error(L"unApply of {}: RegOpenKeyExW failed: {}", toString(), get_last_error_or_default(res));
                return false;
            }
            detail::on_exit closeKey{ [key] { RegCloseKey(key); } };

            // delete the value itself
            if (auto res = RegDeleteKeyValueW(scope, path.c_str(), name.has_value() ? name->c_str() : nullptr); res != ERROR_SUCCESS)
            {
                Logger::error(L"unApply of {}: RegDeleteKeyValueW failed: {}", toString(), get_last_error_or_default(res));
                return false;
            }

            // Check if the path doesn't contain anything and delete it if so
            DWORD nValues = 0;
            DWORD maxValueLen = 0;
            const auto ok =
                RegQueryInfoKeyW(
                    key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &nValues, nullptr, &maxValueLen, nullptr, nullptr) ==
                ERROR_SUCCESS;

            if (ok && (!nValues || !maxValueLen))
            {
                RegDeleteTreeW(scope, path.c_str());
            }
            return true;
        }

        bool requiresElevation() const { return scope == HKEY_LOCAL_MACHINE; }

    private:
        static DWORD valueTypeToWinapiType(const value_t& v)
        {
            return std::visit(
                [](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, DWORD>)
                        return REG_DWORD;
                    else if constexpr (std::is_same_v<T, std::wstring>)
                        return REG_SZ;
                    else
                        static_assert(always_false_v<T>, "support for this registry type is not implemented");
                },
                v);
        }

        static void valueToBuffer(const value_t& value, wchar_t buffer[VALUE_BUFFER_SIZE], DWORD& valueSize, DWORD& type)
        {
            using detail::overloaded;

            std::visit(overloaded{ [&](DWORD value) {
                                      *reinterpret_cast<DWORD*>(buffer) = value;
                                      type = REG_DWORD;
                                      valueSize = sizeof(value);
                                  },
                                   [&](const std::wstring& value) {
                                       assert(value.size() < VALUE_BUFFER_SIZE);
                                       value.copy(buffer, value.size());
                                       type = REG_SZ;
                                       valueSize = static_cast<DWORD>(sizeof(wchar_t) * value.size());
                                   } },
                       value);
        }

        static std::optional<value_t> bufferToValue(const wchar_t buffer[VALUE_BUFFER_SIZE],
                                                    const DWORD valueSize,
                                                    const DWORD type)
        {
            switch (type)
            {
            case REG_DWORD:
                return *(DWORD*)buffer;
            case REG_SZ:
            {
                if (!valueSize)
                {
                    return std::wstring{};
                }
                std::wstring result{ buffer, valueSize / sizeof(wchar_t) };
                while (result[result.size() - 1] == L'\0')
                {
                    result.resize(result.size() - 1);
                }
                return result;
            }
            default:
                return std::nullopt;
            }
        }
    };

    struct ChangeSet
    {
        std::vector<ValueChange> changes;

        bool isApplied() const
        {
            for (const auto& c : changes)
            {
                if (!c.isApplied())
                {
                    return false;
                }
            }
            return true;
        }

        bool apply() const
        {
            bool ok = true;
            for (const auto& c : changes)
            {
                ok = c.apply() && ok;
            }
            return ok;
        }

        bool unApply() const
        {
            bool ok = true;
            for (const auto& c : changes)
            {
                ok = c.unApply() && ok;
            }
            return ok;
        }
    };

    const inline std::wstring DOTNET_COMPONENT_CATEGORY_CLSID = L"{62C8FE65-4EBB-45E7-B440-6E39B2CDBF29}";
    const inline std::wstring ITHUMBNAIL_PROVIDER_CLSID = L"{E357FCCD-A995-4576-B01F-234630154E96}";
    const inline std::wstring IPREVIEW_HANDLER_CLSID = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

    namespace shellex
    {
        enum PreviewHandlerType
        {
            preview,
            thumbnail
        };

        inline registry::ChangeSet generatePreviewHandler(const PreviewHandlerType handlerType,
                                                          const bool perUser,
                                                          std::wstring handlerClsid,
                                                          std::wstring powertoysVersion,
                                                          std::wstring fullPathToHandler,
                                                          std::wstring handlerCategory,
                                                          std::wstring className,
                                                          std::wstring displayName,
                                                          std::wstring fileType)
        {
            const HKEY scope = perUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;

            std::wstring clsidPath = L"Software\\Classes\\CLSID";
            clsidPath += L'\\';
            clsidPath += handlerClsid;

            std::wstring inprocServerPath = clsidPath;
            inprocServerPath += L'\\';
            inprocServerPath += L"InprocServer32";

            std::wstring implementedCategoriesPath = clsidPath + LR"d(\Implemented Categories\)d";
            implementedCategoriesPath += handlerCategory;

            std::wstring assemblyKeyValue;
            if (const auto lastDotPos = className.rfind(L'.'); lastDotPos != std::wstring::npos)
            {
                assemblyKeyValue = L"PowerToys." + className.substr(lastDotPos + 1);
            }
            else
            {
                assemblyKeyValue = L"PowerToys." + className;
            }

            assemblyKeyValue += L", Version=";
            assemblyKeyValue += powertoysVersion;
            assemblyKeyValue += L", Culture=neutral";

            std::wstring versionPath = inprocServerPath;
            versionPath += L'\\';
            versionPath += powertoysVersion;

            std::wstring fileAssociationPath = L"Software\\Classes\\";
            fileAssociationPath += fileType;
            fileAssociationPath += L"\\shellex\\";
            fileAssociationPath += handlerType == PreviewHandlerType::preview ? IPREVIEW_HANDLER_CLSID : ITHUMBNAIL_PROVIDER_CLSID;

            using vec_t = std::vector<registry::ValueChange>;
            // TODO: verify that we actually need all of those
            vec_t changes = { { scope, clsidPath, L"DisplayName", displayName },
                              { scope, clsidPath, std::nullopt, className },
                              { scope, implementedCategoriesPath, std::nullopt, L"" },
                              { scope, inprocServerPath, std::nullopt, fullPathToHandler },
                              { scope, inprocServerPath, L"Assembly", assemblyKeyValue },
                              { scope, inprocServerPath, L"Class", className },
                              { scope, inprocServerPath, L"ThreadingModel", L"Both" },
                              { scope, versionPath, L"Assembly", assemblyKeyValue },
                              { scope, versionPath, L"Class", className },
                              { scope, fileAssociationPath, std::nullopt, handlerClsid } };
            if (handlerType == PreviewHandlerType::preview)
            {
                const std::wstring previewHostClsid = L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";
                const std::wstring previewHandlerListPath = LR"(Software\Microsoft\Windows\CurrentVersion\PreviewHandlers)";

                changes.push_back({ scope, clsidPath, L"AppID", previewHostClsid });
                changes.push_back({ scope, previewHandlerListPath, handlerClsid, displayName });
            }

            return registry::ChangeSet{ .changes = std::move(changes) };
        }
    }
}
