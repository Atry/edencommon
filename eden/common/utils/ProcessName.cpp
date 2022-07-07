/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "eden/common/utils/ProcessName.h"

#include <folly/Exception.h>
#include <folly/FileUtil.h>
#include <folly/lang/ToAscii.h>
#include "eden/common/utils/Handle.h"
#include "eden/common/utils/StringConv.h"

#ifdef __APPLE__
#include <libproc.h> // @manual
#include <sys/sysctl.h> // @manual
#endif

namespace facebook::eden {

namespace detail {

ProcPidCmdLine getProcPidCmdLine(pid_t pid) {
  ProcPidCmdLine path;
  memcpy(path.data(), "/proc/", 6);
  auto digits =
      folly::to_ascii_decimal(path.data() + 6, path.data() + path.size(), pid);
  memcpy(path.data() + 6 + digits, "/cmdline", 9);
  return path;
}

} // namespace detail

namespace {

#ifdef __APPLE__
// This returns 256kb on my system
size_t queryKernArgMax() {
  int mib[2] = {CTL_KERN, KERN_ARGMAX};
  int argmax = 0;
  size_t size = sizeof(argmax);
  folly::checkUnixError(
      sysctl(mib, std::size(mib), &argmax, &size, nullptr, 0),
      "error retrieving KERN_ARGMAX via sysctl");
  XCHECK(argmax > 0) << "KERN_ARGMAX has a negative value!?";
  return size_t(argmax);
}

folly::StringPiece extractCommandLineFromProcArgs(
    const char* procargs,
    size_t len) {
  /* The format of procargs2 is:
     struct procargs2 {
        int argc;
        char [] executable image path;
        char [] null byte padding out to the word size;
        char [] argv0 with null terminator
        char [] argvN with null terminator
        char [] key=val of first env var (with null terminator)
        char [] key=val of second env var (with null terminator)
        ...
  */

  if (UNLIKELY(len < sizeof(int))) {
    // Should be impossible!
    return "<err:EUNDERFLOW>";
  }

  // Fetch the argc value for the target process
  int argCount = 0;
  memcpy(&argCount, procargs, sizeof(argCount));
  if (argCount < 1) {
    return "<err:BOGUS_ARGC>";
  }

  const char* end = procargs + len;
  // Skip over the image path
  const char* cmdline = procargs + sizeof(int);
  // look for NUL byte
  while (cmdline < end) {
    if (*cmdline == 0) {
      break;
    }
    ++cmdline;
  }
  // look for non-NUL byte
  while (cmdline < end) {
    if (*cmdline != 0) {
      break;
    }
    ++cmdline;
  }
  // now cmdline points to the start of the command line

  const char* ptr = cmdline;
  while (argCount > 0 && ptr < end) {
    if (*ptr == 0) {
      if (--argCount == 0) {
        return folly::StringPiece{cmdline, ptr};
      }
    }
    ptr++;
  }

  return folly::StringPiece{cmdline, end};
}

#endif

} // namespace

ProcessName readProcessName(pid_t pid) {
#ifdef __APPLE__
  // a Meyers Singleton to compute and cache this system parameter
  static size_t argMax = queryKernArgMax();

  std::vector<char> args;
  args.resize(argMax);

  char* procargs = args.data();
  size_t len = args.size();

  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  if (sysctl(mib, std::size(mib), procargs, &len, nullptr, 0) == -1) {
    // AFAICT, the sysctl will only fail in situations where the calling
    // process lacks privs to read the args from the target.
    // The errno value is a bland EINVAL in that case.
    // Regardless of the cause, we'd like to try to show something so we
    // fallback to using libproc to retrieve the image filename.

    // libproc is undocumented and unsupported, but the implementation is open
    // source:
    // https://opensource.apple.com/source/xnu/xnu-2782.40.9/libsyscall/wrappers/libproc/libproc.c
    // The return value is 0 on error, otherwise is the length of the buffer.
    // It takes care of overflow/truncation.

    // The buffer must be exactly PROC_PIDPATHINFO_MAXSIZE in size otherwise
    // an EOVERFLOW is generated (even if the buffer is larger!)
    args.resize(PROC_PIDPATHINFO_MAXSIZE);
    ssize_t rv = proc_pidpath(pid, args.data(), PROC_PIDPATHINFO_MAXSIZE);
    if (rv != 0) {
      return std::string{args.data(), args.data() + rv};
    }
    return folly::to<std::string>("<err:", errno, ">");
  }

  // The sysctl won't fail if the buffer is too small, but should set the len
  // value to approximately the used length on success.
  // If the buffer is too small it leaves
  // the value that was passed in as-is.  Therefore we can detect that our
  // buffer was too small if the size is >= the available data space.
  // The returned len in the success case seems to be smaller than the input
  // length.  For example, a successful call with len returned as 1012 requires
  // an input buffer of length 1029
  if (len >= args.size()) {
    return "<err:EOVERFLOW>";
  }

  return extractCommandLineFromProcArgs(procargs, len).str();
#elif _WIN32
  ProcessHandle handle{
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid)};
  if (!handle) {
    auto err = GetLastError();
    return fmt::format(FMT_STRING("<err:{}>"), win32ErrorToString(err));
  }

  // MAX_PATH on Windows is only 260 characters, but on recent Windows, this
  // constant doesn't represent the actual maximum length of a path, since
  // there is no exact value for it, and QueryFullProcessImageName doesn't
  // appear to be helpful in giving us the actual size of the path, we just
  // use a large enough value.
  wchar_t path[SHRT_MAX];
  DWORD size = SHRT_MAX;
  if (QueryFullProcessImageNameW(handle.get(), 0, path, &size) == 0) {
    auto err = GetLastError();
    return fmt::format(FMT_STRING("<err:{}>"), win32ErrorToString(err));
  }

  return wideToMultibyteString<std::string>(path);
#else
  char target[1024];
  const auto fd = folly::openNoInt(
      detail::getProcPidCmdLine(pid).data(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    return folly::to<std::string>("<err:", errno, ">");
  }
  SCOPE_EXIT {
    folly::closeNoInt(fd);
  };

  ssize_t rv = folly::readFull(fd, target, sizeof(target));
  if (rv == -1) {
    return folly::to<std::string>("<err:", errno, ">");
  } else {
    // Could do something fancy if the entire buffer is filled, but it's better
    // if this code does as few syscalls as possible, so just truncate the
    // result.
    return std::string{target, target + rv};
  }
#endif
}

} // namespace facebook::eden
