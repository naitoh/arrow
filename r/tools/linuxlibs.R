# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

args <- commandArgs(TRUE)
VERSION <- args[1]
dst_dir <- paste0("libarrow/arrow-", VERSION)

arrow_repo <- "https://dl.bintray.com/ursalabs/arrow-r/libarrow/"
apache_src_url <- paste0(
  "https://archive.apache.org/dist/arrow/arrow-", VERSION,
  "/apache-arrow-", VERSION, ".tar.gz"
)

options(.arrow.cleanup = character()) # To collect dirs to rm on exit
on.exit(unlink(getOption(".arrow.cleanup")))

env_is <- function(var, value) identical(tolower(Sys.getenv(var)), value)
# * no download, build_ok: Only build with local git checkout
# * download_ok, no build: Only use prebuilt binary, if found
# * neither: Get the arrow-without-arrow package
download_ok <- env_is("LIBARROW_DOWNLOAD", "true") || !(tolower(Sys.getenv("LIBARROW_BINARY")) %in% c("", "false"))
build_ok <- !env_is("LIBARROW_BUILD", "false")
# For local debugging, set ARROW_R_DEV=TRUE to make this script print more
quietly <- !env_is("ARROW_R_DEV", "true")

download_binary <- function(os = identify_os()) {
  libfile <- tempfile()
  if (!is.null(os)) {
    # See if we can map this os-version to one we have binaries for
    os <- find_available_binary(os)
    binary_url <- paste0(arrow_repo, "bin/", os, "/arrow-", VERSION, ".zip")
    try(
      download.file(binary_url, libfile, quiet = quietly),
      silent = quietly
    )
    if (file.exists(libfile)) {
      cat(sprintf("*** Successfully retrieved C++ binaries for %s\n", os))
    } else {
      cat(sprintf("*** No C++ binaries found for %s\n", os))
      libfile <- NULL
    }
  } else {
    libfile <- NULL
  }
  libfile
}

# Function to figure out which flavor of binary we should download, if at all.
# By default (unset or "FALSE"), it will not download a precompiled library,
# but you can override this by setting the env var LIBARROW_BINARY to:
# * `TRUE` (not case-sensitive), to try to discover your current OS, or
# * some other string, presumably a related "distro-version" that has binaries
#   built that work for your OS
identify_os <- function(os = Sys.getenv("LIBARROW_BINARY", Sys.getenv("LIBARROW_DOWNLOAD"))) {
  if (tolower(os) %in% c("", "false")) {
    # Env var says not to download a binary
    return(NULL)
  } else if (!identical(tolower(os), "true")) {
    # Env var provided an os-version to use--maybe you're on Ubuntu 18.10 but
    # we only build for 18.04 and that's fine--so use what the user set
    return(os)
  }

  if (nzchar(Sys.which("lsb_release"))) {
    distro <- tolower(system("lsb_release -is", intern = TRUE))
    os_version <- system("lsb_release -rs", intern = TRUE)
    # In the future, we may be able to do some mapping of distro-versions to
    # versions we built for, since there's no way we'll build for everything.
    os <- paste0(distro, "-", os_version)
  } else if (file.exists("/etc/os-release")) {
    os_release <- readLines("/etc/os-release")
    vals <- sub("^.*=(.*)$", "\\1", os_release)
    names(vals) <- sub("^(.*)=.*$", "\\1", os_release)
    distro <- gsub('"', '', vals["ID"])
    os_version <- "unknown" # default value
    if ("VERSION_ID" %in% names(vals)) {
      if (distro == "ubuntu") {
        # Keep major.minor version
        version_regex <- '^"?([0-9]+\\.[0-9]+).*"?.*$'
      } else {
        # Only major version number
        version_regex <- '^"?([0-9]+).*"?.*$'
      }
      os_version <- sub(version_regex, "\\1", vals["VERSION_ID"])
    } else if ("PRETTY_NAME" %in% names(vals) && grepl("bullseye", vals["PRETTY_NAME"])) {
      # debian unstable doesn't include a number but we can map from pretty name
      os_version <- "11"
    }
    os <- paste0(distro, "-", os_version)
  } else if (file.exists("/etc/system-release")) {
    # Something like "CentOS Linux release 7.7.1908 (Core)"
    system_release <- tolower(utils::head(readLines("/etc/system-release"), 1))
    # Extract from that the distro and the major version number
    os <- sub("^([a-z]+) .* ([0-9]+).*$", "\\1-\\2", system_release)
  } else {
    cat("*** Unable to identify current OS/version\n")
    os <- NULL
  }

  os
}

find_available_binary <- function(os) {
  # Download a csv that maps one to the other, columns "actual" and "use_this"
  u <- "https://raw.githubusercontent.com/ursa-labs/arrow-r-nightly/master/linux/distro-map.csv"
  lookup <- try(utils::read.csv(u, stringsAsFactors = FALSE), silent = quietly)
  if (!inherits(lookup, "try-error") && os %in% lookup$actual) {
    new <- lookup$use_this[lookup$actual == os]
    if (length(new) == 1 && !is.na(new)) { # Just some sanity checking
      cat(sprintf("*** Using %s binary for %s\n", new, os))
      os <- new
    }
  }
  os
}

download_source <- function() {
  tf1 <- tempfile()
  src_dir <- NULL
  source_url <- paste0(arrow_repo, "src/arrow-", VERSION, ".zip")
  try(
    download.file(source_url, tf1, quiet = quietly),
    silent = quietly
  )
  if (!file.exists(tf1)) {
    # Try for an official release
    try(
      download.file(apache_src_url, tf1, quiet = quietly),
      silent = quietly
    )
  }
  if (file.exists(tf1)) {
    cat("*** Successfully retrieved C++ source\n")
    src_dir <- tempfile()
    unzip(tf1, exdir = src_dir)
    unlink(tf1)
    # These scripts need to be executable
    system(sprintf("chmod 755 %s/cpp/build-support/*.sh", src_dir))
    options(.arrow.cleanup = c(getOption(".arrow.cleanup"), src_dir))
    # The actual src is in cpp
    src_dir <- paste0(src_dir, "/cpp")
  }
  src_dir
}

find_local_source <- function(arrow_home = Sys.getenv("ARROW_HOME", "..")) {
  if (file.exists(paste0(arrow_home, "/cpp/src/arrow/api.h"))) {
    # We're in a git checkout of arrow, so we can build it
    cat("*** Found local C++ source\n")
    return(paste0(arrow_home, "/cpp"))
  } else {
    return(NULL)
  }
}

build_libarrow <- function(src_dir, dst_dir) {
  # We'll need to compile R bindings with these libs, so delete any .o files
  system("rm src/*.o", ignore.stdout = quietly, ignore.stderr = quietly)
  # Set up make for parallel building
  makeflags <- Sys.getenv("MAKEFLAGS")
  if (makeflags == "") {
    makeflags <- sprintf("-j%s", parallel::detectCores())
    Sys.setenv(MAKEFLAGS = makeflags)
  }
  if (!quietly) {
    cat("*** Building with MAKEFLAGS=", makeflags, "\n")
  }
  # Check for libarrow build dependencies:
  # * cmake
  cmake <- ensure_cmake()

  build_dir <- tempfile()
  options(.arrow.cleanup = c(getOption(".arrow.cleanup"), build_dir))
  env_vars <- sprintf(
    "SOURCE_DIR=%s BUILD_DIR=%s DEST_DIR=%s CMAKE=%s",
    src_dir,       build_dir,   dst_dir,    cmake
  )
  cat("**** arrow", ifelse(quietly, "", paste("with", env_vars)), "\n")
  system(
    paste(env_vars, "inst/build_arrow_static.sh"),
    ignore.stdout = quietly, ignore.stderr = quietly
  )
}

ensure_cmake <- function() {
  cmake <- Sys.which("cmake")
  if (!nzchar(cmake)) {
    # If not found, download it
    cat("**** cmake\n")
    CMAKE_VERSION <- Sys.getenv("CMAKE_VERSION", "3.16.2")
    cmake_binary_url <- paste0(
      "https://github.com/Kitware/CMake/releases/download/v", CMAKE_VERSION,
      "/cmake-", CMAKE_VERSION, "-Linux-x86_64.tar.gz"
    )
    cmake_tar <- tempfile()
    cmake_dir <- tempfile()
    try(
      download.file(cmake_binary_url, cmake_tar, quiet = quietly),
      silent = quietly
    )
    untar(cmake_tar, exdir = cmake_dir)
    unlink(cmake_tar)
    options(.arrow.cleanup = c(getOption(".arrow.cleanup"), cmake_dir))
    cmake <- paste0(
      cmake_dir,
      "/cmake-", CMAKE_VERSION, "-Linux-x86_64",
      "/bin/cmake"
    )
  }
  cmake
}

#####

if (!file.exists(paste0(dst_dir, "/include/arrow/api.h"))) {
  # If we're working in a local checkout and have already built the libs, we
  # don't need to do anything. Otherwise,
  # (1) Look for a prebuilt binary for this version
  bin_file <- src_dir <- NULL
  if (download_ok) {
    bin_file <- download_binary()
  }
  if (!is.null(bin_file)) {
    # Extract them
    dir.create(dst_dir, showWarnings = !quietly, recursive = TRUE)
    unzip(bin_file, exdir = dst_dir)
    unlink(bin_file)
  } else if (build_ok) {
    # (2) Find source and build it
    if (download_ok) {
      src_dir <- download_source()
    }
    if (is.null(src_dir)) {
      src_dir <- find_local_source()
    }
    if (!is.null(src_dir)) {
      cat("*** Building C++ libraries\n")
      build_libarrow(src_dir, dst_dir)
    } else {
      cat("*** Proceeding without C++ dependencies\n")
    }
  } else {
   cat("*** Proceeding without C++ dependencies\n")
  }
}
