// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <sstream>
#include <utility>

#include "arrow/filesystem/filesystem.h"
#ifdef ARROW_HDFS
#include "arrow/filesystem/hdfs.h"
#endif
#ifdef ARROW_S3
#include "arrow/filesystem/s3fs.h"
#endif
#include "arrow/filesystem/localfs.h"
#include "arrow/filesystem/mockfs.h"
#include "arrow/filesystem/path_util.h"
#include "arrow/filesystem/util_internal.h"
#include "arrow/io/slow.h"
#include "arrow/result.h"
#include "arrow/status.h"
#include "arrow/util/logging.h"
#include "arrow/util/macros.h"
#include "arrow/util/uri.h"
#include "arrow/util/windows_fixup.h"

namespace arrow {

using internal::Uri;

namespace fs {

using internal::ConcatAbstractPath;
using internal::EnsureTrailingSlash;
using internal::GetAbstractPathParent;
using internal::kSep;
using internal::RemoveLeadingSlash;
using internal::RemoveTrailingSlash;
using internal::ToSlashes;

std::string ToString(FileType ftype) {
  switch (ftype) {
    case FileType::NonExistent:
      return "non-existent";
    case FileType::Unknown:
      return "unknown";
    case FileType::File:
      return "file";
    case FileType::Directory:
      return "directory";
    default:
      ARROW_LOG(FATAL) << "Invalid FileType value: " << static_cast<int>(ftype);
      return "???";
  }
}

// For googletest
ARROW_EXPORT std::ostream& operator<<(std::ostream& os, FileType ftype) {
#define FILE_TYPE_CASE(value_name)                  \
  case FileType::value_name:                        \
    os << "FileType::" ARROW_STRINGIFY(value_name); \
    break;

  switch (ftype) {
    FILE_TYPE_CASE(NonExistent)
    FILE_TYPE_CASE(Unknown)
    FILE_TYPE_CASE(File)
    FILE_TYPE_CASE(Directory)
    default:
      ARROW_LOG(FATAL) << "Invalid FileType value: " << static_cast<int>(ftype);
  }

#undef FILE_TYPE_CASE
  return os;
}

std::string FileInfo::base_name() const {
  return internal::GetAbstractPathParent(path_).second;
}

std::string FileInfo::dir_name() const {
  return internal::GetAbstractPathParent(path_).first;
}

// Debug helper
std::string FileInfo::ToString() const {
  std::stringstream os;
  os << *this;
  return os.str();
}

std::ostream& operator<<(std::ostream& os, const FileInfo& info) {
  return os << "FileInfo(" << info.type() << ", " << info.path() << ")";
}

std::string FileInfo::extension() const {
  return internal::GetAbstractPathExtension(path_);
}

//////////////////////////////////////////////////////////////////////////
// FileSystem default method implementations

FileSystem::~FileSystem() {}

Result<std::string> FileSystem::NormalizePath(std::string path) { return path; }

Result<std::vector<FileInfo>> FileSystem::GetTargetInfos(
    const std::vector<std::string>& paths) {
  std::vector<FileInfo> res;
  res.reserve(paths.size());
  for (const auto& path : paths) {
    ARROW_ASSIGN_OR_RAISE(FileInfo info, GetTargetInfo(path));
    res.push_back(std::move(info));
  }
  return res;
}

Status FileSystem::DeleteFiles(const std::vector<std::string>& paths) {
  Status st = Status::OK();
  for (const auto& path : paths) {
    st &= DeleteFile(path);
  }
  return st;
}

//////////////////////////////////////////////////////////////////////////
// SubTreeFileSystem implementation

SubTreeFileSystem::SubTreeFileSystem(const std::string& base_path,
                                     std::shared_ptr<FileSystem> base_fs)
    : base_path_(NormalizeBasePath(base_path, base_fs).ValueOrDie()), base_fs_(base_fs) {}

SubTreeFileSystem::~SubTreeFileSystem() {}

Result<std::string> SubTreeFileSystem::NormalizeBasePath(
    std::string base_path, const std::shared_ptr<FileSystem>& base_fs) {
  ARROW_ASSIGN_OR_RAISE(base_path, base_fs->NormalizePath(std::move(base_path)));
  return EnsureTrailingSlash(std::move(base_path));
}

std::string SubTreeFileSystem::PrependBase(const std::string& s) const {
  if (s.empty()) {
    return base_path_;
  } else {
    return ConcatAbstractPath(base_path_, s);
  }
}

Status SubTreeFileSystem::PrependBaseNonEmpty(std::string* s) const {
  if (s->empty()) {
    return Status::IOError("Empty path");
  } else {
    *s = ConcatAbstractPath(base_path_, *s);
    return Status::OK();
  }
}

Result<std::string> SubTreeFileSystem::StripBase(const std::string& s) const {
  auto len = base_path_.length();
  // Note base_path_ ends with a slash (if not empty)
  if (s.length() >= len && s.substr(0, len) == base_path_) {
    return s.substr(len);
  } else {
    return Status::UnknownError("Underlying filesystem returned path '", s,
                                "', which is not a subpath of '", base_path_, "'");
  }
}

Status SubTreeFileSystem::FixInfo(FileInfo* info) const {
  ARROW_ASSIGN_OR_RAISE(auto fixed_path, StripBase(info->path()));
  info->set_path(std::move(fixed_path));
  return Status::OK();
}

Result<std::string> SubTreeFileSystem::NormalizePath(std::string path) {
  ARROW_ASSIGN_OR_RAISE(auto normalized, base_fs_->NormalizePath(PrependBase(path)));
  return StripBase(std::move(normalized));
}

Result<FileInfo> SubTreeFileSystem::GetTargetInfo(const std::string& path) {
  ARROW_ASSIGN_OR_RAISE(FileInfo info, base_fs_->GetTargetInfo(PrependBase(path)));
  RETURN_NOT_OK(FixInfo(&info));
  return info;
}

Result<std::vector<FileInfo>> SubTreeFileSystem::GetTargetInfos(
    const FileSelector& select) {
  auto selector = select;
  selector.base_dir = PrependBase(selector.base_dir);
  ARROW_ASSIGN_OR_RAISE(auto infos, base_fs_->GetTargetInfos(selector));
  for (auto& info : infos) {
    RETURN_NOT_OK(FixInfo(&info));
  }
  return infos;
}

Status SubTreeFileSystem::CreateDir(const std::string& path, bool recursive) {
  auto s = path;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  return base_fs_->CreateDir(s, recursive);
}

Status SubTreeFileSystem::DeleteDir(const std::string& path) {
  auto s = path;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  return base_fs_->DeleteDir(s);
}

Status SubTreeFileSystem::DeleteDirContents(const std::string& path) {
  auto s = PrependBase(path);
  return base_fs_->DeleteDirContents(s);
}

Status SubTreeFileSystem::DeleteFile(const std::string& path) {
  auto s = path;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  return base_fs_->DeleteFile(s);
}

Status SubTreeFileSystem::Move(const std::string& src, const std::string& dest) {
  auto s = src;
  auto d = dest;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  RETURN_NOT_OK(PrependBaseNonEmpty(&d));
  return base_fs_->Move(s, d);
}

Status SubTreeFileSystem::CopyFile(const std::string& src, const std::string& dest) {
  auto s = src;
  auto d = dest;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  RETURN_NOT_OK(PrependBaseNonEmpty(&d));
  return base_fs_->CopyFile(s, d);
}

Result<std::shared_ptr<io::InputStream>> SubTreeFileSystem::OpenInputStream(
    const std::string& path) {
  auto s = path;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  return base_fs_->OpenInputStream(s);
}

Result<std::shared_ptr<io::RandomAccessFile>> SubTreeFileSystem::OpenInputFile(
    const std::string& path) {
  auto s = path;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  return base_fs_->OpenInputFile(s);
}

Result<std::shared_ptr<io::OutputStream>> SubTreeFileSystem::OpenOutputStream(
    const std::string& path) {
  auto s = path;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  return base_fs_->OpenOutputStream(s);
}

Result<std::shared_ptr<io::OutputStream>> SubTreeFileSystem::OpenAppendStream(
    const std::string& path) {
  auto s = path;
  RETURN_NOT_OK(PrependBaseNonEmpty(&s));
  return base_fs_->OpenAppendStream(s);
}

//////////////////////////////////////////////////////////////////////////
// SlowFileSystem implementation

SlowFileSystem::SlowFileSystem(std::shared_ptr<FileSystem> base_fs,
                               std::shared_ptr<io::LatencyGenerator> latencies)
    : base_fs_(base_fs), latencies_(latencies) {}

SlowFileSystem::SlowFileSystem(std::shared_ptr<FileSystem> base_fs,
                               double average_latency)
    : base_fs_(base_fs), latencies_(io::LatencyGenerator::Make(average_latency)) {}

SlowFileSystem::SlowFileSystem(std::shared_ptr<FileSystem> base_fs,
                               double average_latency, int32_t seed)
    : base_fs_(base_fs), latencies_(io::LatencyGenerator::Make(average_latency, seed)) {}

Result<FileInfo> SlowFileSystem::GetTargetInfo(const std::string& path) {
  latencies_->Sleep();
  return base_fs_->GetTargetInfo(path);
}

Result<std::vector<FileInfo>> SlowFileSystem::GetTargetInfos(
    const FileSelector& selector) {
  latencies_->Sleep();
  return base_fs_->GetTargetInfos(selector);
}

Status SlowFileSystem::CreateDir(const std::string& path, bool recursive) {
  latencies_->Sleep();
  return base_fs_->CreateDir(path, recursive);
}

Status SlowFileSystem::DeleteDir(const std::string& path) {
  latencies_->Sleep();
  return base_fs_->DeleteDir(path);
}

Status SlowFileSystem::DeleteDirContents(const std::string& path) {
  latencies_->Sleep();
  return base_fs_->DeleteDirContents(path);
}

Status SlowFileSystem::DeleteFile(const std::string& path) {
  latencies_->Sleep();
  return base_fs_->DeleteFile(path);
}

Status SlowFileSystem::Move(const std::string& src, const std::string& dest) {
  latencies_->Sleep();
  return base_fs_->Move(src, dest);
}

Status SlowFileSystem::CopyFile(const std::string& src, const std::string& dest) {
  latencies_->Sleep();
  return base_fs_->CopyFile(src, dest);
}

Result<std::shared_ptr<io::InputStream>> SlowFileSystem::OpenInputStream(
    const std::string& path) {
  latencies_->Sleep();
  ARROW_ASSIGN_OR_RAISE(auto stream, base_fs_->OpenInputStream(path));
  return std::make_shared<io::SlowInputStream>(stream, latencies_);
}

Result<std::shared_ptr<io::RandomAccessFile>> SlowFileSystem::OpenInputFile(
    const std::string& path) {
  latencies_->Sleep();
  ARROW_ASSIGN_OR_RAISE(auto file, base_fs_->OpenInputFile(path));
  return std::make_shared<io::SlowRandomAccessFile>(file, latencies_);
}

Result<std::shared_ptr<io::OutputStream>> SlowFileSystem::OpenOutputStream(
    const std::string& path) {
  latencies_->Sleep();
  // XXX Should we have a SlowOutputStream that waits on Flush() and Close()?
  return base_fs_->OpenOutputStream(path);
}

Result<std::shared_ptr<io::OutputStream>> SlowFileSystem::OpenAppendStream(
    const std::string& path) {
  latencies_->Sleep();
  return base_fs_->OpenAppendStream(path);
}

namespace {

struct FileSystemUri {
  Uri uri;
  std::string scheme;
  std::string path;
  bool is_local = false;
};

Result<FileSystemUri> ParseFileSystemUri(const std::string& uri_string) {
  FileSystemUri fsuri;
  auto status = fsuri.uri.Parse(uri_string);
  if (!status.ok()) {
#ifdef _WIN32
    // Could be a "file:..." URI with backslashes instead of regular slashes.
    RETURN_NOT_OK(fsuri.uri.Parse(ToSlashes(uri_string)));
    if (fsuri.uri.scheme() != "file") {
      return status;
    }
#else
    return status;
#endif
  }
  fsuri.scheme = fsuri.uri.scheme();
  fsuri.path = fsuri.uri.path();
  if (fsuri.scheme == "file") {
    fsuri.is_local = true;
  }
  return std::move(fsuri);
}

Result<FileSystemUri> ParseFileSystemUriOrPath(const std::string& uri_string) {
  if (internal::DetectAbsolutePath(uri_string)) {
    FileSystemUri fsuri;
    fsuri.path = uri_string;
    fsuri.is_local = true;
    return std::move(fsuri);
  }
  return ParseFileSystemUri(uri_string);
}

Result<std::shared_ptr<FileSystem>> FileSystemFromUriReal(const FileSystemUri& fsuri,
                                                          const std::string& uri_string,
                                                          std::string* out_path) {
  if (out_path != nullptr) {
    *out_path = fsuri.path;
  }
  if (fsuri.is_local) {
    // Normalize path separators
    if (out_path != nullptr) {
      *out_path = ToSlashes(*out_path);
    }
    return std::make_shared<LocalFileSystem>();
  }
  if (fsuri.scheme == "hdfs" || fsuri.scheme == "viewfs") {
#ifdef ARROW_HDFS
    ARROW_ASSIGN_OR_RAISE(auto options, HdfsOptions::FromUri(fsuri.uri));
    ARROW_ASSIGN_OR_RAISE(auto hdfs, HadoopFileSystem::Make(options));
    return hdfs;
#else
    return Status::NotImplemented("Got HDFS URI but Arrow compiled without HDFS support");
#endif
  }
  if (fsuri.scheme == "s3") {
#ifdef ARROW_S3
    RETURN_NOT_OK(EnsureS3Initialized());
    ARROW_ASSIGN_OR_RAISE(auto options, S3Options::FromUri(fsuri.uri, out_path));
    ARROW_ASSIGN_OR_RAISE(auto s3fs, S3FileSystem::Make(options));
    return s3fs;
#else
    return Status::NotImplemented("Got S3 URI but Arrow compiled without S3 support");
#endif
  }

  // Other filesystems below do not have an absolute / relative path distinction,
  // normalize path by removing leading slash.
  // XXX perhaps each filesystem should have a path normalization method?
  if (out_path != nullptr) {
    *out_path = std::string(RemoveLeadingSlash(*out_path));
  }
  if (fsuri.scheme == "mock") {
    return std::make_shared<internal::MockFileSystem>(internal::CurrentTimePoint());
  }

  return Status::Invalid("Unrecognized filesystem type in URI: ", uri_string);
}

}  // namespace

Result<std::shared_ptr<FileSystem>> FileSystemFromUri(const std::string& uri_string,
                                                      std::string* out_path) {
  ARROW_ASSIGN_OR_RAISE(auto fsuri, ParseFileSystemUri(uri_string));
  return FileSystemFromUriReal(fsuri, uri_string, out_path);
}

Result<std::shared_ptr<FileSystem>> FileSystemFromUriOrPath(const std::string& uri_string,
                                                            std::string* out_path) {
  ARROW_ASSIGN_OR_RAISE(auto fsuri, ParseFileSystemUriOrPath(uri_string));
  auto maybe_fs = FileSystemFromUriReal(fsuri, uri_string, out_path);
  if (maybe_fs.ok()) {
    return maybe_fs;
  }
  return Status::Invalid("Expected URI or absolute local path, got '", uri_string, "'");
}

Status FileSystemFromUri(const std::string& uri, std::shared_ptr<FileSystem>* out_fs,
                         std::string* out_path) {
  return FileSystemFromUri(uri, out_path).Value(out_fs);
}

}  // namespace fs
}  // namespace arrow
