/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "boot_event_record_store.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <utime.h>
#include <cstdlib>
#include <string>
#include <utility>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include "histogram_logger.h"
#include "uptime_parser.h"

namespace {

const char BOOTSTAT_DATA_DIR[] = "/data/misc/bootstat/";

// Given a boot even record file at |path|, extracts the event's relative time
// from the record into |uptime|.
bool ParseRecordEventTime(const std::string& path, int32_t* uptime) {
  DCHECK_NE(static_cast<int32_t*>(nullptr), uptime);

  struct stat file_stat;
  if (stat(path.c_str(), &file_stat) == -1) {
    PLOG(ERROR) << "Failed to read " << path;
    return false;
  }

  *uptime = file_stat.st_mtime;

  // The following code (till function exit) is a debug test to ensure the
  // validity of the file mtime value, i.e., to check that the record file
  // mtime values are not changed once set.
  // TODO(jhawkins): Remove this code.
  std::string content;
  if (!android::base::ReadFileToString(path, &content)) {
    PLOG(ERROR) << "Failed to read " << path;
    return false;
  }

  // Ignore existing bootstat records (which do not contain file content).
  if (!content.empty()) {
    int32_t value;
    if (android::base::ParseInt(content, &value)) {
      bootstat::LogHistogram("bootstat_mtime_matches_content", value == *uptime);
    }
  }

  return true;
}

}  // namespace

BootEventRecordStore::BootEventRecordStore() {
  SetStorePath(BOOTSTAT_DATA_DIR);
}

void BootEventRecordStore::AddBootEvent(const std::string& event) {
  AddBootEventWithValue(event, bootstat::ParseUptime());
}

// The implementation of AddBootEventValue makes use of the mtime file
// attribute to store the value associated with a boot event in order to
// optimize on-disk size requirements and small-file thrashing.
void BootEventRecordStore::AddBootEventWithValue(
    const std::string& event, int32_t value) {
  std::string record_path = GetBootEventPath(event);
  int record_fd = creat(record_path.c_str(), S_IRUSR | S_IWUSR);
  if (record_fd == -1) {
    PLOG(ERROR) << "Failed to create " << record_path;
    return;
  }

  // Writing the value as content in the record file is a debug measure to
  // ensure the validity of the file mtime value, i.e., to check that the record
  // file mtime values are not changed once set.
  // TODO(jhawkins): Remove this block.
  if (!android::base::WriteStringToFd(std::to_string(value), record_fd)) {
    PLOG(ERROR) << "Failed to write value to " << record_path;
    close(record_fd);
    return;
  }

  // Fill out the stat structure for |record_path| in order to get the atime to
  // set in the utime() call.
  struct stat file_stat;
  if (stat(record_path.c_str(), &file_stat) == -1) {
    PLOG(ERROR) << "Failed to read " << record_path;
    close(record_fd);
    return;
  }

  // Set the |modtime| of the file to store the value of the boot event while
  // preserving the |actime| (as read by stat).
  struct utimbuf times = {/* actime */ file_stat.st_atime, /* modtime */ value};
  if (utime(record_path.c_str(), &times) == -1) {
    PLOG(ERROR) << "Failed to set mtime for " << record_path;
    close(record_fd);
    return;
  }

  close(record_fd);
}

bool BootEventRecordStore::GetBootEvent(
    const std::string& event, BootEventRecord* record) const {
  CHECK_NE(static_cast<BootEventRecord*>(nullptr), record);
  CHECK(!event.empty());

  const std::string record_path = GetBootEventPath(event);
  int32_t uptime;
  if (!ParseRecordEventTime(record_path, &uptime)) {
    LOG(ERROR) << "Failed to parse boot time record: " << record_path;
    return false;
  }

  *record = std::make_pair(event, uptime);
  return true;
}

std::vector<BootEventRecordStore::BootEventRecord> BootEventRecordStore::
    GetAllBootEvents() const {
  std::vector<BootEventRecord> events;

  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(store_path_.c_str()), closedir);

  // This case could happen due to external manipulation of the filesystem,
  // so crash out if the record store doesn't exist.
  CHECK_NE(static_cast<DIR*>(nullptr), dir.get());

  struct dirent* entry;
  while ((entry = readdir(dir.get())) != NULL) {
    // Only parse regular files.
    if (entry->d_type != DT_REG) {
      continue;
    }

    const std::string event = entry->d_name;
    BootEventRecord record;
    if (!GetBootEvent(event, &record)) {
      LOG(ERROR) << "Failed to parse boot time event: " << event;
      continue;
    }

    events.push_back(record);
  }

  return events;
}

void BootEventRecordStore::SetStorePath(const std::string& path) {
  DCHECK_EQ('/', path.back());
  store_path_ = path;
}

std::string BootEventRecordStore::GetBootEventPath(
    const std::string& event) const {
  DCHECK_EQ('/', store_path_.back());
  return store_path_ + event;
}
