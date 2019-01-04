/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "install/fuse_sdcard_install.h"

#include <dirent.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <cutils/properties.h>

#include "bootloader_message/bootloader_message.h"
#include "fuse_provider.h"
#include "fuse_sideload.h"
#include "install/install.h"
#include "otautil/roots.h"

#define UFS_DEV_SDCARD_BLK_PATH "/dev/block/mmcblk0p1"

using android::volmgr::VolumeInfo;
using android::volmgr::VolumeManager;

// How long (in seconds) we wait for the fuse-provided package file to
// appear, before timing out.
static constexpr int SDCARD_INSTALL_TIMEOUT = 10;

// Set the BCB to reboot back into recovery (it won't resume the install from
// sdcard though).
static void SetSdcardUpdateBootloaderMessage() {
  std::vector<std::string> options;
  std::string err;
  if (!update_bootloader_message(options, &err)) {
    LOG(ERROR) << "Failed to set BCB message: " << err;
  }
}

// Returns the selected filename, or an empty string.
static std::string BrowseDirectory(const std::string& path, Device* device, RecoveryUI* ui) {
  std::unique_ptr<DIR, decltype(&closedir)> d(opendir(path.c_str()), closedir);
  if (!d) {
    PLOG(ERROR) << "error opening " << path;
    return "";
  }

  std::vector<std::string> dirs;
  std::vector<std::string> entries{ "../" };  // "../" is always the first entry.

  dirent* de;
  while ((de = readdir(d.get())) != nullptr) {
    std::string name(de->d_name);

    if (de->d_type == DT_DIR) {
      // Skip "." and ".." entries.
      if (name == "." || name == "..") continue;
      dirs.push_back(name + "/");
    } else if (de->d_type == DT_REG && android::base::EndsWithIgnoreCase(name, ".zip")) {
      entries.push_back(name);
    }
  }

  std::sort(dirs.begin(), dirs.end());
  std::sort(entries.begin(), entries.end());

  // Append dirs to the entries list.
  entries.insert(entries.end(), dirs.begin(), dirs.end());

  std::vector<std::string> headers{ "Choose a package to install:", path };

  size_t chosen_item = 0;
  while (true) {
    chosen_item = ui->ShowMenu(
        headers, entries, chosen_item, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

    // Return if WaitKey() was interrupted.
    if (chosen_item == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
      return "";
    }
    if (chosen_item == Device::kGoHome) {
      return "@";
    }
    if (chosen_item == Device::kGoBack || chosen_item == 0) {
      // Go up but continue browsing (if the caller is browse_directory).
      return "";
    }

    const std::string& item = entries[chosen_item];

    std::string new_path = path + "/" + item;
    if (new_path.back() == '/') {
      // Recurse down into a subdirectory.
      new_path.pop_back();
      std::string result = BrowseDirectory(new_path, device, ui);
      if (!result.empty()) return result;
    } else {
      // Selected a zip file: return the path to the caller.
      return new_path;
    }
  }

  // Unreachable.
}

static bool StartSdcardFuse(const std::string& path) {
  auto file_data_reader = std::make_unique<FuseFileDataProvider>(path, 65536);

  if (!file_data_reader->Valid()) {
    return false;
  }

  return run_fuse_sideload(std::move(file_data_reader)) == 0;
}

static int is_ufs_dev() {
  char bootdevice[PROPERTY_VALUE_MAX] = {0};
  property_get("ro.boot.bootdevice", bootdevice, "N/A");
  LOG(ERROR) << "ro.boot.bootdevice is: " << bootdevice << "\n";
  if (strlen(bootdevice) < strlen(".ufshc") + 1)
    return 0;
  return (!strncmp(&bootdevice[strlen(bootdevice) - strlen(".ufshc")],
                   ".ufshc",
                   sizeof(".ufshc")));
}

static int do_sdcard_mount_for_ufs() {
  int rc = 0;
  LOG(ERROR) << "Update via sdcard on UFS dev.Mounting card\n";
  Volume *v = volume_for_mount_point("/sdcard");
  if (v == nullptr) {
    LOG(ERROR) << "Unknown volume for /sdcard.Check fstab\n";
    goto error;
  }
  if (strncmp(v->fs_type.c_str(), "vfat", sizeof("vfat"))) {
    LOG(ERROR) << "Unsupported format on the sdcard: "
               << v->fs_type.c_str() << "\n";
    goto error;
  }
  rc = mount(UFS_DEV_SDCARD_BLK_PATH,
             v->mount_point.c_str(),
             v->fs_type.c_str(),
             v->flags,
             v->fs_options.c_str());
  if (rc) {
    LOG(ERROR) << "Failed to mount sdcard: " << strerror(errno) << "\n";
    goto error;
  }
  LOG(ERROR) << "Done mounting sdcard\n";
  return 0;
error:
  return -1;
}

int ApplyFromStorage(Device* device, VolumeInfo& vi, RecoveryUI* ui) {
  if (is_ufs_dev()) {
    if (do_sdcard_mount_for_ufs() != 0) {
      LOG(ERROR) << "\nFailed to mount sdcard\n";
      return INSTALL_ERROR;
    }
  } else {
  if (!VolumeManager::Instance()->volumeMount(vi.mId)) {
    return INSTALL_ERROR;
    }
  }

  std::string path = BrowseDirectory(vi.mPath, device, ui);
  if (path == "@") {
    return INSTALL_NONE;
  }

  if (path.empty()) {
    VolumeManager::Instance()->volumeUnmount(vi.mId);
    return INSTALL_NONE;
  }

  ui->Print("\n-- Install %s ...\n", path.c_str());
  SetSdcardUpdateBootloaderMessage();

  // We used to use fuse in a thread as opposed to a process. Since accessing
  // through fuse involves going from kernel to userspace to kernel, it leads
  // to deadlock when a page fault occurs. (Bug: 26313124)
  pid_t child;
  if ((child = fork()) == 0) {
    bool status = StartSdcardFuse(path);

    _exit(status ? EXIT_SUCCESS : EXIT_FAILURE);
  }

  // FUSE_SIDELOAD_HOST_PATHNAME will start to exist once the fuse in child
  // process is ready.
  int result = INSTALL_ERROR;
  int status;
  bool waited = false;
  for (int i = 0; i < SDCARD_INSTALL_TIMEOUT; ++i) {
    if (waitpid(child, &status, WNOHANG) == -1) {
      result = INSTALL_ERROR;
      waited = true;
      break;
    }

    struct stat sb;
    if (stat(FUSE_SIDELOAD_HOST_PATHNAME, &sb) == -1) {
      if (errno == ENOENT && i < SDCARD_INSTALL_TIMEOUT - 1) {
        sleep(1);
        continue;
      } else {
        LOG(ERROR) << "Timed out waiting for the fuse-provided package.";
        result = INSTALL_ERROR;
        kill(child, SIGKILL);
        break;
      }
    }

    result = install_package(FUSE_SIDELOAD_HOST_PATHNAME, false, true, 0 /*retry_count*/,
                             true /* verify */, false /* allow_ab_downgrade */, ui);
    if (result == INSTALL_UNVERIFIED && ask_to_continue_unverified(device)) {
      result = install_package(FUSE_SIDELOAD_HOST_PATHNAME, false, false, 0 /*retry_count*/,
                               false /* verify */, false /* allow_ab_downgrade */, ui);
    }
    if (result == INSTALL_DOWNGRADE && ask_to_continue_downgrade(device)) {
      result = install_package(FUSE_SIDELOAD_HOST_PATHNAME, false, false, 0 /*retry_count*/,
                               false /* verify */, true /* allow_ab_downgrade */, ui);
    }

    break;
  }

  if (!waited) {
    // Calling stat() on this magic filename signals the fuse
    // filesystem to shut down.
    struct stat sb;
    stat(FUSE_SIDELOAD_HOST_EXIT_PATHNAME, &sb);

    waitpid(child, &status, 0);
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    LOG(ERROR) << "Error exit from the fuse process: " << WEXITSTATUS(status);
  }

  VolumeManager::Instance()->volumeUnmount(vi.mId);
  return result;
}
