#include <string>
#include <cstring>
#include <string_view>
#include <set>
#include <atomic>
#include <unordered_map>
#include <cstdint>
#include <sys/mount.h>
#include <elfio/elfio.hpp>

// These includes are from the system_properties submodule, not NDK!
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <api/system_properties.h>
#include <api/_system_properties.h>
#include <system_properties/prop_info.h>

#include "zygisk.hpp"
#include "logging.hpp"
#include "map_parser.hpp"
#include "mountinfo_parser.hpp"
#include "utils.hpp"

using namespace Parsers;

static const std::set<std::string> fsname_list = {"KSU", "APatch", "magisk", "worker"};
static const std::unordered_map<std::string, int> mount_flags_procfs = {
    {"nosuid", MS_NOSUID},
    {"nodev", MS_NODEV},
    {"noexec", MS_NOEXEC},
    {"noatime", MS_NOATIME},
    {"nodiratime", MS_NODIRATIME},
    {"relatime", MS_RELATIME},
    {"nosymfollow", MS_NOSYMFOLLOW}};

static bool shouldUnmount(const mountinfo_entry &mount, const mountinfo_root_resolver &root_resolver)
{
    const auto true_root = root_resolver.resolveRootOf(mount);
    const auto &mount_point = mount.getMountPoint();
    const auto &type = mount.getFilesystemType();

    // Mount is from /data/adb
    if (true_root.starts_with("/data/adb"))
        return true;

    // Mount is to /data/adb
    if (mount_point.starts_with("/data/adb"))
        return true;

    // Unmount all module overlayfs and tmpfs
    if ((type == "overlay" || type == "tmpfs" || type == "devpts") && fsname_list.contains(mount.getMountSource()))
        return true;

    // Unmount all overlayfs with lowerdir/upperdir/workdir starting with /data/adb
    if (type == "overlay")
    {
        const auto &options = mount.getSuperOptions();

        if (options.contains("lowerdir") && options.at("lowerdir").starts_with("/data/adb"))
            return true;

        if (options.contains("upperdir") && options.at("upperdir").starts_with("/data/adb"))
            return true;

        if (options.contains("workdir") && options.at("workdir").starts_with("/data/adb"))
            return true;
    }

    return false;
}

void doUnmount()
{
    const auto &mount_infos = parseSelfMountinfo(false);
    auto root_resolver = mountinfo_root_resolver(mount_infos);

    for (auto it = mount_infos.rbegin(); it != mount_infos.rend(); it++)
    {
        if (shouldUnmount(*it, root_resolver))
        {
            const auto &mount_point_cstr = it->getMountPoint().c_str();
            if (umount2(mount_point_cstr, MNT_DETACH) == 0)
                LOGD("umount2(\"%s\", MNT_DETACH) returned 0", mount_point_cstr);
            else
                LOGW("umount2(\"%s\", MNT_DETACH) returned -1: %d (%s)", mount_point_cstr, errno, strerror(errno));
        }
    }
}

void doRemount()
{
    for (const auto &mount : parseSelfMountinfo(false))
    {
        if (mount.getMountPoint() == "/data")
        {
            const auto &superOptions = mount.getSuperOptions();
            if (!superOptions.contains("errors"))
                break;

            // Remount /data only if errors behavior is not the same as superblock's
            const char *sb_errors = Utils::getExtErrorsBehavior(mount);
            if (!sb_errors || superOptions.at("errors") == sb_errors)
                break;

            const auto &mountOptions = mount.getMountOptions();
            unsigned long flags = MS_REMOUNT;
            for (const auto &flagName : mount_flags_procfs)
            {
                if (mountOptions.contains(flagName.first))
                    flags |= flagName.second;
            }

            if (::mount(NULL, "/data", NULL, flags, (std::string("errors=") + sb_errors).c_str()) == 0)
                LOGD("mount(NULL, \"/data\", NULL, 0x%lx, ...) returned 0", flags);
            else
                LOGW("mount(NULL, \"/data\", NULL, 0x%lx, ...) returned -1: %d (%s)", flags, errno, strerror(errno));
            break;
        }
    }
}

/*
 * Is it guaranteed to work? No.
 * At least it has lots of error checking so if something goes wrong
 * the state should remain relatively safe.
 */
void doHideZygisk()
{
    using namespace ELFIO;

    elfio reader;
    std::string filePath;
    uintptr_t startAddress = 0, bssAddress = 0;

    for (const auto &map : parseSelfMaps())
    {
        if (map.getPathname().ends_with("/libnativebridge.so") && map.getPerms() == "r--p")
        {
            // First ro page should be the ELF header
            filePath = map.getPathname();
            startAddress = map.getAddressStart();
            break;
        }
    }

    ASSERT_DO(doHideZygisk, startAddress != 0, return);
    ASSERT_DO(doHideZygisk, reader.load(filePath), return);

    size_t bssSize = 0;
    for (const auto &sec : reader.sections)
    {
        if (sec->get_name() == ".bss")
        {
            bssAddress = startAddress + sec->get_address();
            bssSize = static_cast<size_t>(sec->get_size());
            break;
        }
    }

    ASSERT_DO(doHideZygisk, bssAddress != 0, return);
    LOGD("Found .bss for \"%s\" at 0x%" PRIxPTR " sized %" PRIuPTR " bytes.", filePath.c_str(), bssAddress, bssSize);

    uint8_t *pHadError = reinterpret_cast<uint8_t *>(memchr(reinterpret_cast<void *>(bssAddress), 0x01, bssSize));
    if (pHadError != nullptr)
    {
        *pHadError = 0;
        LOGD("libnativebridge.so had_error was reset.");
    }
}

static bool shouldResetProperty(const prop_info *pi)
{
    // Read-write properties or properties with long values should not be reset
    if (strncmp(pi->name, "ro.", 3) != 0 || pi->is_long())
        return false;

    // Check if the serial indicates that it was modified
    auto serial = std::atomic_load_explicit(&pi->serial, std::memory_order_relaxed);
    if ((serial & 0xFFFFFF) != 0)
        return true;

    // Check if any characters exist beyond the null-terminated string
    size_t length = strnlen(pi->value, PROP_VALUE_MAX);
    for (size_t i = length; i < PROP_VALUE_MAX; i++)
    {
        if (pi->value[i] != '\0')
            return true;
    }

    return false;
}

void doMrProp()
{
    static bool isInitialized = false;
    static int resetCount = 0;
    if (!isInitialized)
    {
        if (__system_properties_init() == -1)
        {
            LOGE("Could not initialize system_properties!");
            return;
        }
        isInitialized = true;
    }

    int ret = __system_property_foreach(
        [](const prop_info *pi, void *)
        {
            if (shouldResetProperty(pi))
            {
                // Overlapping pointers in strncpy is undefined behavior so make a copy.
                char buffer[PROP_VALUE_MAX];
                size_t length = Utils::safeStringCopy(buffer, pi->value, PROP_VALUE_MAX);

                __system_property_update(const_cast<prop_info *>(pi), buffer, length);
                resetCount++;
            }
        },
        nullptr);

    LOGD("__system_property_foreach returned %d. resetCount=%d", ret, resetCount);
}
