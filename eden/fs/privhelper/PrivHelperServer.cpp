/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#ifndef _WIN32

#include "eden/fs/privhelper/PrivHelperServer.h"
#include "eden/fs/privhelper/PrivHelperConn.h"

#include <boost/algorithm/string/predicate.hpp>
#include <fcntl.h>
#include <folly/Exception.h>
#include <folly/Expected.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/Utility.h>
#include <folly/init/Init.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/LogConfigParser.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/xlog.h>
#include <folly/portability/Unistd.h>
#include <folly/system/ThreadName.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <chrono>
#include <csignal>
#include <set>
#include "eden/common/utils/PathFuncs.h"
#include "eden/common/utils/SysctlUtil.h"
#include "eden/common/utils/Throw.h"
#include "eden/fs/privhelper/NfsMountRpc.h"
#include "eden/fs/privhelper/priority/ProcessPriority.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h> // @manual
#include <IOKit/kext/KextManager.h> // @manual
#include <eden/common/utils/Pipe.h>
#include <eden/common/utils/SpawnedProcess.h>
#include <fuse_ioctl.h> // @manual
#include <fuse_mount.h> // @manual
#include <grp.h> // @manual
#include <sys/ioccom.h> // @manual
#include <sys/sysctl.h> // @manual
#endif

using folly::checkUnixError;
using folly::IOBuf;
using folly::throwSystemError;
using folly::io::Appender;
using folly::io::Cursor;
using folly::io::RWPrivateCursor;
using std::string;

namespace facebook::eden {

// Constants
static constexpr folly::StringPiece kFamBinaryPath{
    "/usr/local/libexec/eden/edenfs_fam/SCMFileAccessMonitor.app/Contents/MacOS/SCMFileAccessMonitor"};

PrivHelperServer::PrivHelperServer() = default;

PrivHelperServer::~PrivHelperServer() = default;

void PrivHelperServer::init(folly::File socket, uid_t uid, gid_t gid) {
  initPartial(std::move(socket), uid, gid);
}

void PrivHelperServer::initPartial(folly::File socket, uid_t uid, gid_t gid) {
  // Make sure init() is only called once.
  XCHECK_EQ(uid_, std::numeric_limits<uid_t>::max());
  XCHECK_EQ(gid_, std::numeric_limits<gid_t>::max());

  // Set our thread name to to make it easier to distinguish
  // the privhelper process from the main EdenFS process.  Setting the thread
  // name for the main thread also changes the process name reported
  // /proc/PID/comm (and therefore by ps).
  //
  // Note that the process name is limited to 15 bytes on Linux, so our process
  // name shows up only as "edenfs_privhelp"
  folly::setThreadName("edenfs_privhelper");

  // eventBase_ is a unique_ptr only so that we can delay constructing it until
  // init() is called.  We want to avoid creating it in the constructor since
  // the constructor is often called before we fork, and the EventBase
  // NotificationQueue code checks to ensure that it isn't used across a fork.
  eventBase_ = std::make_unique<folly::EventBase>();
  conn_ = UnixSocket::makeUnique(eventBase_.get(), std::move(socket));
  uid_ = uid;
  gid_ = gid;

  folly::checkPosixError(chdir("/"), "privhelper failed to chdir(/)");
}

#ifdef __APPLE__
namespace {

std::pair<int, int> determineMacOsVersion() {
  auto version = getSysCtlByName("kern.osproductversion", 64);

  int major, minor, patch;
  if (sscanf(version.c_str(), "%d.%d.%d", &major, &minor, &patch) < 2) {
    folly::throwSystemErrorExplicit(
        EINVAL, "failed to parse kern.osproductversion string ", version);
  }

  return std::make_pair(major, minor);
}

std::string computeOSXFuseKextPath() {
  auto version = determineMacOsVersion();
  // Starting from Big Sur (macOS 11), we no longer need to look for the second
  // number since it is now a _real_ minor version number.
  if (version.first >= 11) {
    return folly::to<std::string>(
        OSXFUSE_EXTENSIONS_PATH, "/", version.first, "/", OSXFUSE_KEXT_NAME);
  }
  return folly::to<std::string>(
      OSXFUSE_EXTENSIONS_PATH,
      "/",
      version.first,
      ".",
      version.second,
      "/",
      OSXFUSE_KEXT_NAME);
}

std::string computeEdenFsKextPath() {
  auto version = determineMacOsVersion();
  return folly::to<std::string>(
      "/Library/Filesystems/eden.fs/Contents/Extensions/",
      version.first,
      ".",
      version.second,
      "/edenfs.kext");
}

// Returns true if the system already knows about the fuse filesystem stuff
bool shouldLoadOSXFuseKext() {
  struct vfsconf vfc;
  return getvfsbyname("osxfuse", &vfc) != 0;
}

bool shouldLoadEdenFsKext() {
  struct vfsconf vfc;
  return getvfsbyname("edenfs", &vfc) != 0;
}

constexpr folly::StringPiece kNfsExtensionPath =
    "/System/Library/Extensions/nfs.kext";

bool shouldLoadNfsKext() {
  if (access(kNfsExtensionPath.str().c_str(), F_OK) != 0) {
    XLOGF(
        DBG3,
        "Kernel extension does not exist at '{}', skipping",
        kNfsExtensionPath);
    return false;
  }

  struct vfsconf vfc;
  return getvfsbyname("nfs", &vfc) != 0;
}

bool tryLoadKext(const std::string& kextPathString) {
  CFStringRef kextPath = CFStringCreateWithCString(
      kCFAllocatorDefault, kextPathString.c_str(), kCFStringEncodingUTF8);
  SCOPE_EXIT {
    CFRelease(kextPath);
  };

  CFURLRef kextUrl = CFURLCreateWithFileSystemPath(
      kCFAllocatorDefault, kextPath, kCFURLPOSIXPathStyle, true);
  SCOPE_EXIT {
    CFRelease(kextUrl);
  };

  auto ret = KextManagerLoadKextWithURL(kextUrl, NULL);

  if (ret != kOSReturnSuccess) {
    XLOGF(ERR, "Failed to load {}: error code {}", kextPathString, ret);
    // Soft error: we might be able to continue with MacFuse
    return false;
  }

  return true;
}

void updateOSXFuseAdminGroup() {
  // libfuse uses a sysctl to update the kext's idea of the admin group,
  // so we do too!
  auto adminGroup = getgrnam(MACOSX_ADMIN_GROUP_NAME);
  if (adminGroup) {
    int gid = adminGroup->gr_gid;
    sysctlbyname(OSXFUSE_SYSCTL_TUNABLES_ADMIN, NULL, NULL, &gid, sizeof(gid));
  }
}

bool loadNfsKext() {
  return tryLoadKext(kNfsExtensionPath.str());
}

// The osxfuse kernel doesn't automatically assign a device, so we have
// to loop through the different units and attempt to allocate them,
// one by one.  Returns the fd and its unit number on success, throws
// an exception on error.
std::pair<folly::File, int> allocateFuseDevice(bool useDevEdenFs) {
  if (useDevEdenFs) {
    if (shouldLoadEdenFsKext()) {
      tryLoadKext(computeEdenFsKextPath());
      updateOSXFuseAdminGroup();
    }
  } else if (shouldLoadOSXFuseKext()) {
    tryLoadKext(computeOSXFuseKextPath());
    updateOSXFuseAdminGroup();
  }

  int fd = -1;
  const int nDevices = OSXFUSE_NDEVICES;
  int dindex;
  for (dindex = 0; dindex < nDevices; dindex++) {
    auto devName = folly::to<std::string>(
        useDevEdenFs ? "/dev/edenfs" : "/dev/osxfuse", dindex);
    fd = folly::openNoInt(devName.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      return std::make_pair(folly::File{fd, true}, dindex);
    }

    if (errno == EBUSY) {
      continue;
    }
    if (errno == ENODEV || errno == ENOENT) {
      throwSystemError(
          "failed to open ",
          devName,
          ": make sure the osxfuse kernel module is loaded");
    } else {
      throwSystemError("failed to open ", devName);
    }
  }

  throwSystemError(
      "unable to allocate an osxfuse device, "
      "either all instances are busy or the kernel module is not loaded");
}

template <typename T, std::size_t Size>
void checkThenPlaceInBuffer(T (&buf)[Size], folly::StringPiece data) {
  if (data.size() >= Size) {
    throwf<std::runtime_error>(
        "string exceeds buffer size in snprintf.  result was {}", data);
  }

  memcpy(buf, data.data(), data.size());
  buf[data.size()] = '\0';
}

// Mount osxfuse (3.x)
folly::File mountOSXFuse(
    const char* mountPath,
    bool readOnly,
    std::chrono::nanoseconds fuseTimeout,
    bool useDevEdenFs) {
  auto [fuseDev, dindex] = allocateFuseDevice(useDevEdenFs);

  fuse_mount_args args{};
  auto canonicalPath = ::realpath(mountPath, NULL);
  if (!canonicalPath) {
    folly::throwSystemError("failed to realpath ", mountPath);
  }
  SCOPE_EXIT {
    free(canonicalPath);
  };
  if (strlen(canonicalPath) >= sizeof(args.mntpath) - 1) {
    folly::throwSystemErrorExplicit(
        EINVAL, "mount path ", canonicalPath, " is too large for args.mntpath");
  }
  strcpy(args.mntpath, canonicalPath);

  // The most important part of the osxfuse mount protocol is to prove
  // to the mount() syscall that we own an opened unit.  We do this by
  // copying the rdev from the fd and by performing a magic ioctl to
  // get a magic cookie and putting both of those values into the
  // fuse_mount_args struct.
  struct stat st;
  checkUnixError(fstat(fuseDev.fd(), &st));
  args.rdev = st.st_rdev;

  checkUnixError(
      ioctl(fuseDev.fd(), FUSEDEVIOCGETRANDOM, &args.random),
      "failed negotiation with ioctl FUSEDEVIOCGETRANDOM");

  // We get to set some metadata for for mounted volume
  checkThenPlaceInBuffer(
      args.fsname,
      fmt::format(
          "eden@{}{}",
          useDevEdenFs ? "edenfs" : OSXFUSE_DEVICE_BASENAME,
          dindex));
  args.altflags |= FUSE_MOPT_FSNAME;

  auto mountPathBaseName = basename(canonicalPath);
  checkThenPlaceInBuffer(args.volname, mountPathBaseName);
  args.altflags |= FUSE_MOPT_VOLNAME;

  checkThenPlaceInBuffer(args.fstypename, "eden");
  args.altflags |= FUSE_MOPT_FSTYPENAME;

  // And some misc other options...

  args.blocksize = FUSE_DEFAULT_BLOCKSIZE;
  args.altflags |= FUSE_MOPT_BLOCKSIZE;

  // The daemon timeout is a hard timeout for fuse request processing.
  // If the timeout is reached, the kernel will shut down the fuse
  // connection.
  auto daemon_timeout_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(fuseTimeout).count();
  if (daemon_timeout_seconds > FUSE_MAX_DAEMON_TIMEOUT) {
    args.daemon_timeout = FUSE_MAX_DAEMON_TIMEOUT;
  } else {
    args.daemon_timeout = daemon_timeout_seconds;
  }
  XLOGF(
      ERR,
      "Max daemon timeout ({}) exceeded. Setting daemon_timeout to {}",
      FUSE_MAX_DAEMON_TIMEOUT,
      args.daemon_timeout);
  args.altflags |= FUSE_MOPT_DAEMON_TIMEOUT;

  // maximum iosize for reading or writing.  We want to allow a much
  // larger default than osxfuse normally provides so that clients
  // can minimize the number of read(2)/write(2) calls needed to
  // write a given chunk of data.
  args.iosize = 1024 * 1024;
  args.altflags |= FUSE_MOPT_IOSIZE;

  // We want normal unix permissions semantics; do not blanket deny
  // access to !owner.  Do not send access(2) calls to userspace.
  args.altflags |= FUSE_MOPT_ALLOW_OTHER | FUSE_MOPT_DEFAULT_PERMISSIONS;

  int mountFlags = MNT_NOSUID;
  if (readOnly) {
    mountFlags |= MNT_RDONLY;
  }

  // The mount() syscall can internally attempt to interrogate the filesystem
  // before it returns to us here.  We can't respond to those requests
  // until we have passed the device back to the dispatcher so we're forced
  // to do a little asynchronous dance and run the mount in a separate
  // thread.
  // We'd like to be able to catch invalid parameters detected by mount;
  // those are likely to be immediately returned to us, so we commit a
  // minor crime here and allow the mount thread to set the errno into
  // a shared value.
  // Then we can wait for a short grace period to see if that got populated
  // with an error and propagate that.
  auto shared_errno = std::make_shared<std::atomic<int>>(0);

  auto thr =
      std::thread([args, mountFlags, useDevEdenFs, shared_errno]() mutable {
        auto devName = useDevEdenFs ? "edenfs" : OSXFUSE_NAME;
        auto res = mount(devName, args.mntpath, mountFlags, &args);
        if (res != 0) {
          *shared_errno = errno;
          XLOGF(
              ERR,
              "failed to mount {} using {}: {}",
              args.mntpath,
              devName,
              folly::errnoStr(*shared_errno));
        }
      });
  thr.detach();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (*shared_errno) {
    folly::throwSystemErrorExplicit(
        *shared_errno, "mount failed for ", args.mntpath);
  }

  return std::move(fuseDev);
}

// Mount MacFuse (4.x)
// MacFuse is a closed-source fork of osxfuse.  In 4.x the mount procedure
// became opaque behind a loader utility that performs the actual mount syscall
// using an undocumented and backwards incompatible mount protocol to prior
// versions. This function uses that utility to perform the mount procedure.
folly::File mountMacFuse(
    const char* mountPath,
    bool readOnly,
    std::chrono::nanoseconds fuseTimeout) {
  if (readOnly) {
    folly::throwSystemErrorExplicit(
        EINVAL, "MacFUSE doesn't support read-only mounts");
  }

  // mount_macfuse will send the fuse device descriptor back to us
  // over a unix domain socket; we create the connected pair here
  // and pass the descriptor to mount_macfuse via the _FUSE_COMMFD
  // environment variable below.
  SocketPair socketPair;
  SpawnedProcess::Options opts;

  auto commFd = opts.inheritDescriptor(std::move(socketPair.write));
  // mount_macfuse refuses to do anything unless this is set
  opts.environment().set("_FUSE_CALL_BY_LIB", "1");
  // Tell it which unix socket to use to pass back the device
  opts.environment().set("_FUSE_COMMFD", folly::to<std::string>(commFd));
  // Tell it to use version 2 of the mount protocol
  opts.environment().set("_FUSE_COMMVERS", "2");
  // It is unclear what purpose passing the daemon path serves, but
  // libfuse does this, and thus we do also.
  opts.environment().set("_FUSE_DAEMON_PATH", executablePath().asString());

  AbsolutePath canonicalPath = realpath(mountPath);

  // These options are equivalent to those that are explained in more
  // detail in mountOSXFuse() above.
  std::vector<std::string> args = {
      "/Library/Filesystems/macfuse.fs/Contents/Resources/mount_macfuse",
      "-ofsname=eden",
      fmt::format("-ovolname={}", canonicalPath.basename()),
      "-ofstypename=eden",
      fmt::format("-oblocksize={}", FUSE_DEFAULT_BLOCKSIZE),
      fmt::format(
          "-odaemon_timeout={}",
          std::chrono::duration_cast<std::chrono::seconds>(fuseTimeout)
              .count()),
      fmt::format("-oiosize={}", 1024 * 1024),
      "-oallow_other",
      "-odefault_permissions",
      canonicalPath.asString(),
  };

  // Start the helper...
  SpawnedProcess mounter(args, std::move(opts));
  // ... but wait for it in another thread.
  // We MUST NOT try to wait for it directly here as the mount protocol
  // requires FUSE_INIT to be replied to before the mount_macfuse can
  // return, and attempting to disrupt that can effectively deadlock
  // macOS to the point that you need to powercycle!
  // We move the process wait into a separate thread so that it can
  // take its time to wait on the child process.
  auto thr =
      std::thread([proc = std::move(mounter)]() mutable { proc.wait(); });
  // we can't wait for the thread for the same reason, so detach it.
  thr.detach();

  // Now, prepare to receive the fuse device descriptor via our socketpair.
  struct iovec iov;
  char buf[1];
  char ccmsg[CMSG_SPACE(sizeof(int))];

  iov.iov_base = buf;
  iov.iov_len = sizeof(buf);

  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ccmsg;
  msg.msg_controllen = sizeof(ccmsg);

  while (1) {
    auto rv = recvmsg(socketPair.read.fd(), &msg, 0);
    if (rv == -1 && errno == EINTR) {
      continue;
    }
    if (rv == -1) {
      folly::throwSystemErrorExplicit(
          errno, "failed to recvmsg the fuse device descriptor from MacFUSE");
    }
    if (rv == 0) {
      folly::throwSystemErrorExplicit(
          ECONNRESET,
          "failed to recvmsg the fuse device descriptor from MacFUSE");
    }
    break;
  }

  auto cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg->cmsg_type != SCM_RIGHTS) {
    folly::throwSystemErrorExplicit(
        EINVAL,
        "MacFUSE didn't send SCM_RIGHTS message while transferring fuse device descriptor");
  }

  // Got it; copy the bytes into something with the right type
  int fuseDevice;
  memcpy(&fuseDevice, CMSG_DATA(cmsg), sizeof(fuseDevice));

  // and take ownership!
  // The caller will complete the FUSE_INIT handshake.
  return folly::File{fuseDevice, true};
}

} // namespace
#endif

folly::File PrivHelperServer::fuseMount(
    const char* mountPath,
    bool readOnly,
    [[maybe_unused]] const char* vfsType) {
#ifdef __APPLE__
  if (useDevEdenFs_) {
    return mountOSXFuse(mountPath, readOnly, fuseTimeout_, useDevEdenFs_);
  }

  try {
    return mountMacFuse(mountPath, readOnly, fuseTimeout_);
  } catch (const std::exception& macFuseExc) {
    XLOGF(
        ERR,
        "Failed to mount using MacFuse, trying OSXFuse ({})",
        folly::exceptionStr(macFuseExc));
    return mountOSXFuse(mountPath, readOnly, fuseTimeout_, useDevEdenFs_);
  }
#else
  // We manually call open() here rather than using the folly::File()
  // constructor just so we can emit a slightly more helpful message on error.
  const char* devName = "/dev/fuse";
  const int fd = folly::openNoInt(devName, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENODEV || errno == ENOENT) {
      throwSystemError(
          "failed to open ",
          devName,
          ": make sure the fuse kernel module is loaded");
    } else {
      throwSystemError("failed to open ", devName);
    }
  }
  folly::File fuseDev(fd, true);

  // Prepare the flags and options to pass to mount(2).
  // We currently don't allow these to be customized by the unprivileged
  // requester.  We could add this functionality in the future if we have a
  // need for it, but we would need to validate their changes are safe.
  const int rootMode = S_IFDIR;
  auto mountOpts = fmt::format(
      "allow_other,default_permissions,"
      "rootmode={:o},user_id={},group_id={},fd={}",
      rootMode,
      uid_,
      gid_,
      fuseDev.fd());

  // The mount flags.
  // We do not use MS_NODEV.  MS_NODEV prevents mount points from being created
  // inside our filesystem.  We currently use bind mounts to point the buck-out
  // directory to an alternate location outside of eden.
  int mountFlags = MS_NOSUID;
  if (readOnly) {
    mountFlags |= MS_RDONLY;
  }
  // The colon indicates to coreutils/gnulib that this is a remote
  // mount so it will not be displayed by `df --local`.
  int rc = mount("edenfs:", mountPath, vfsType, mountFlags, mountOpts.c_str());
  checkUnixError(rc, "failed to mount");
  return fuseDev;
#endif
}

void PrivHelperServer::nfsMount(
    std::string mountPath,
    NFSMountOptions options) {
#ifdef __APPLE__
  if (shouldLoadNfsKext()) {
    XLOG(DBG3, "Apple nfs.kext is not loaded. Attempting to load");
    loadNfsKext();
  }

  // Hold the attribute list set below.
  auto attrsBuf = folly::IOBufQueue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender attrSer{&attrsBuf, 1024};

  // Holds the NFS_MATTR_* flags. Each set flag will have a corresponding
  // structure serialized in the attribute list, the order of serialization
  // must follow the increasing order of their associated flags.
  uint32_t mattrFlags = 0;

  // Check if we should use a soft or hard mount. If soft is desired, set the
  // flag value to NFS_MFLAG_SOFT so it's used in the bitwise OR operation.
  std::string softStr = "hard";
  uint32_t soft_flag = 0;
  if (options.useSoftMount) {
    softStr = "soft";
    soft_flag = NFS_MFLAG_SOFT;
  }

  // unmask_dumbtimer indicates whether we should mask/unmask the dumbtimer
  // value bit in the nfs_matter_flags (i.e. nfs_flag_set) struct.
  // dumbtimer_flag indicates the actual value that should be masked/unmasked
  // depending on the value of unmask_dumbtimer.
  //
  // This separate mask value is needed because dumbtimer is not always passed
  // as a mount option, and therefore the value bit may be invalid.
  uint32_t dumbtimer_flag = 0;
  uint32_t unmask_dumbtimer = 0;
  std::string dumbtimerStr = "nodumbtimer,";
  if (options.dumbtimer.has_value()) {
    unmask_dumbtimer = NFS_MFLAG_DUMBTIMER;
    if (options.dumbtimer.value()) {
      dumbtimer_flag = NFS_MFLAG_DUMBTIMER;
      dumbtimerStr = "dumbtimer,";
    }
  }

  // Check if we should enable readdirplus. If so, set readdirplus to
  // NFS_MFLAG_RDIRPLUS.
  std::string rdirplusStr = "nordirplus";
  uint32_t readdirplus_flag = 0;
  if (options.useReaddirplus) {
    rdirplusStr = "rdirplus";
    readdirplus_flag = NFS_MFLAG_RDIRPLUS;
  }

  /*
   * The flag set does the following:
   *
   * Makes the client use any source port
   * Enables or disables rdirplus (readdirplus) based on EdenConfig value
   * Sets the mount type to soft/hard (but make it interruptible) based on an
   *   EdenConfig value. While in theory we would always want the mount to be
   *   soft, macOS force a maximum timeout of 60s, which in some case is too
   *   short for files to be fetched, thus make it configurable.
   * Possibly specifies dumbtimer behavior, iff an EdenConfig value is
   *   explicitly set.
   *
   * See `man mount_nfs` for more options.
   */
  mattrFlags |= NFS_MATTR_FLAGS;
  nfs_mattr_flags flags{
      NFS_MATTR_BITMAP_LEN,
      NFS_MFLAG_RESVPORT | NFS_MFLAG_RDIRPLUS | NFS_MFLAG_SOFT |
          NFS_MFLAG_INTR | unmask_dumbtimer,
      NFS_MATTR_BITMAP_LEN,
      NFS_MFLAG_INTR | readdirplus_flag | soft_flag | dumbtimer_flag};
  XdrTrait<nfs_mattr_flags>::serialize(attrSer, flags);

  mattrFlags |= NFS_MATTR_NFS_VERSION;
  XdrTrait<nfs_mattr_nfs_version>::serialize(attrSer, 3);

  mattrFlags |= NFS_MATTR_READ_SIZE;
  mattrFlags |= NFS_MATTR_WRITE_SIZE;
  XdrTrait<nfs_mattr_rsize>::serialize(attrSer, options.readIOSize);
  XdrTrait<nfs_mattr_wsize>::serialize(attrSer, options.writeIOSize);

  std::string dsizeStr = "";
  if (options.directoryReadSize.has_value()) {
    dsizeStr = fmt::format("dsize={},", options.directoryReadSize.value());
    mattrFlags |= NFS_MATTR_READDIR_SIZE;
    XdrTrait<nfs_mattr_readdirsize>::serialize(
        attrSer, options.directoryReadSize.value());
  }

  std::string readAheadStr = "";
  readAheadStr = fmt::format("readahead={},", options.readAheadSize);
  mattrFlags |= NFS_MATTR_READAHEAD;
  XdrTrait<nfs_mattr_readahead>::serialize(attrSer, options.readAheadSize);

  // For NFSv2/v3 mounts, perform all file locking operations locally on the NFS
  // client (in the VFS layer) instead of on the NFS server.  This option can
  // provide file locking support on an NFS file system for which the server
  // does not support file locking. However, because the file locking is only
  // performed on the client, the NFS server and other NFS clients will have no
  // knowledge of the locks.
  mattrFlags |= NFS_MATTR_LOCK_MODE;
  XdrTrait<nfs_mattr_lock_mode>::serialize(
      attrSer, nfs_lock_mode::NFS_LOCK_MODE_LOCAL);

  auto mountdFamily = options.mountdAddr.getFamily();
  auto nfsdFamily = options.nfsdAddr.getFamily();
  if (mountdFamily != nfsdFamily) {
    throwf<std::runtime_error>(
        "The mountd and nfsd socket must be of the same type: mountd=\"{}\", nfsd=\"{}\"",
        options.mountdAddr.describe(),
        options.nfsdAddr.describe());
  }

  mattrFlags |= NFS_MATTR_SOCKET_TYPE;
  nfs_mattr_socket_type socketType;
  switch (nfsdFamily) {
    case AF_INET:
      socketType = "tcp4";
      break;
    case AF_INET6:
      socketType = "tcp6";
      break;
    case AF_UNIX:
      socketType = "ticotsord";
      break;
    default:
      throwf<std::runtime_error>("Unknown socket family: {}", nfsdFamily);
  }
  XdrTrait<nfs_mattr_socket_type>::serialize(attrSer, socketType);

  if (options.nfsdAddr.isFamilyInet()) {
    mattrFlags |= NFS_MATTR_NFS_PORT;
    XdrTrait<nfs_mattr_nfs_port>::serialize(
        attrSer, options.nfsdAddr.getPort());

    mattrFlags |= NFS_MATTR_MOUNT_PORT;
    XdrTrait<nfs_mattr_mount_port>::serialize(
        attrSer, options.mountdAddr.getPort());
  }

  // NOTE: This code currently has a bug that prevents timeouts from being set
  // to non-multiples of 10 deciseconds (i.e. a timeout of 25 deciseconds will
  // result in a mount with a timeout of 20 deciseconds). This should be
  // investigated and fixed in the future.
  //
  // Initial RPC request timeout value in tenths of a second, so 10 = 1s.
  std::string retransTimeoutStr = "";
  retransTimeoutStr =
      fmt::format("timeo={},", options.retransmitTimeoutTenthSeconds);
  mattrFlags |= NFS_MATTR_REQUEST_TIMEOUT;
  int32_t retransSeconds = options.retransmitTimeoutTenthSeconds / 10;
  uint32_t retransRemainderNanoseconds =
      (options.retransmitTimeoutTenthSeconds % 10) * 100000000;
  XdrTrait<nfs_mattr_request_timeout>::serialize(
      attrSer,
      nfstime32{
          /*seconds=*/retransSeconds,
          /*nseconds=*/retransRemainderNanoseconds});

  // Indicates the max RPC retransmissions for soft mounts
  std::string retransAttemptsStr = "";
  retransAttemptsStr = fmt::format("retrans={},", options.retransmitAttempts);
  mattrFlags |= NFS_MATTR_SOFT_RETRY_COUNT;
  XdrTrait<nfs_mattr_soft_retry_count>::serialize(
      attrSer, options.retransmitAttempts);

  std::string deadTimeoutStr = "";
  deadTimeoutStr = fmt::format("deadtimeout={}", options.deadTimeoutSeconds);
  mattrFlags |= NFS_MATTR_DEAD_TIMEOUT;
  XdrTrait<nfs_mattr_dead_timeout>::serialize(
      attrSer,
      nfstime32{/*seconds=*/options.deadTimeoutSeconds, /*nseconds=*/0});

  mattrFlags |= NFS_MATTR_FS_LOCATIONS;
  auto path = canonicalPath(mountPath);
  auto componentIterator = path.components();
  std::vector<std::string> components;
  for (const auto component : componentIterator) {
    components.push_back(std::string(component.value()));
  }
  nfs_fs_server server{"edenfs", {}, std::nullopt};
  if (options.nfsdAddr.isFamilyInet()) {
    server.nfss_address.push_back(options.nfsdAddr.getAddressStr());
  } else {
    server.nfss_address.push_back(options.nfsdAddr.getPath());
  }
  nfs_fs_location location{{server}, components};
  nfs_mattr_fs_locations locations{{location}, std::nullopt};
  XdrTrait<nfs_mattr_fs_locations>::serialize(attrSer, locations);

  mattrFlags |= NFS_MATTR_MNTFLAGS;
  // These are non-NFS specific and will be also passed directly to mount(2)
  nfs_mattr_mntflags mountFlags = MNT_NOSUID;
  if (options.readOnly) {
    mountFlags |= MNT_RDONLY;
  }
  XdrTrait<nfs_mattr_mntflags>::serialize(attrSer, mountFlags);

  mattrFlags |= NFS_MATTR_MNTFROM;
  nfs_mattr_mntfrom serverName = "edenfs:";
  XdrTrait<nfs_mattr_mntfrom>::serialize(attrSer, serverName);

  if (options.nfsdAddr.getFamily() == AF_UNIX) {
    mattrFlags |= NFS_MATTR_LOCAL_NFS_PORT;
    XdrTrait<std::string>::serialize(attrSer, options.nfsdAddr.getPath());

    mattrFlags |= NFS_MATTR_LOCAL_MOUNT_PORT;
    XdrTrait<std::string>::serialize(attrSer, options.mountdAddr.getPath());
  }

  auto mountBuf = folly::IOBufQueue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender ser(&mountBuf, 1024);

  nfs_mattr mattr{NFS_MATTR_BITMAP_LEN, mattrFlags, attrsBuf.move()};

  nfs_mount_args args{
      /*args_version*/ 88,
      /*args_length*/ 0,
      /*xdr_args_version*/ NFS_XDRARGS_VERSION_0,
      /*nfs_mount_attrs*/ std::move(mattr),
  };

  auto argsLength = XdrTrait<nfs_mount_args>::serializedSize(args);
  args.args_length = folly::to_narrow(argsLength);

  XdrTrait<nfs_mount_args>::serialize(ser, args);

  auto buf = mountBuf.move();
  buf->coalesce();

  XLOGF(
      DBG1,
      "Mounting {} via NFS with opts: mountaddr={},addr={},rsize={},wsize={},{},{},vers=3,{}{}{}{}{}{}",
      mountPath,
      options.mountdAddr.describe(),
      options.nfsdAddr.describe(),
      options.readIOSize,
      options.writeIOSize,
      softStr,
      rdirplusStr,
      dumbtimerStr,
      dsizeStr,
      readAheadStr,
      retransTimeoutStr,
      retransAttemptsStr,
      deadTimeoutStr);

  int rc = mount("nfs", mountPath.c_str(), mountFlags, (void*)buf->data());
  checkUnixError(rc, "failed to mount");

  /*
   * The fsctl syscall is completely undocumented, but it does contain a way to
   * override the f_fstypename returned by statfs. This allows watchman to
   * properly detects the filesystem as EdenFS and not NFS (watchman refuses to
   * watch an NFS filesystem).
   */
  typedef char fstypename_t[MFSTYPENAMELEN];
#define FSIOC_SET_FSTYPENAME_OVERRIDE _IOW('A', 10, fstypename_t)
#define FSCTL_SET_FSTYPENAME_OVERRIDE IOCBASECMD(FSIOC_SET_FSTYPENAME_OVERRIDE)

  rc = fsctl(
      mountPath.c_str(), FSCTL_SET_FSTYPENAME_OVERRIDE, (void*)"edenfs:", 0);
  if (rc != 0) {
    unmount(mountPath.c_str(), {});
    checkUnixError(rc, "failed to fsctl");
  }

#else
  if (!options.mountdAddr.isFamilyInet() || !options.nfsdAddr.isFamilyInet()) {
    folly::throwSystemErrorExplicit(
        EINVAL,
        fmt::format(
            "only inet addresses are supported: mountdAddr=\"{}\", nfsdAddr=\"{}\"",
            options.mountdAddr.describe(),
            options.nfsdAddr.describe()));
  }
  // Prepare the flags and options to pass to mount(2).
  // Since each mount point will have its own NFS server, we need to manually
  // specify it.
  folly::StringPiece noReaddirplusStr = ",nordirplus,";
  if (options.useReaddirplus) {
    noReaddirplusStr = ",";
  }

  // Check if we should use a soft or hard mount.
  // https://linux.die.net/man/5/nfs
  folly::StringPiece softOptionStr = "hard";
  if (options.useSoftMount) {
    softOptionStr = "soft";
  }

  auto mountOpts = fmt::format(
      "addr={},vers=3,proto=tcp,port={},mountvers=3,mountproto=tcp,mountport={},"
      "noresvport,nolock{}{},retrans={},timeo={},rsize={},wsize={}",
      options.nfsdAddr.getAddressStr(),
      options.nfsdAddr.getPort(),
      options.mountdAddr.getPort(),
      noReaddirplusStr,
      softOptionStr,
      options.retransmitAttempts,
      options.retransmitTimeoutTenthSeconds,
      options.readIOSize,
      options.writeIOSize);

  // The mount flags.
  // We do not use MS_NODEV.  MS_NODEV prevents mount points from being created
  // inside our filesystem.  We currently use bind mounts to point the buck-out
  // directory to an alternate location outside of eden.
  int mountFlags = MS_NOSUID;
  if (options.readOnly) {
    mountFlags |= MS_RDONLY;
  }
  auto source = fmt::format("edenfs:{}", mountPath);
  XLOGF(DBG1, "Mounting {} va NFS with opts: {}", source, mountOpts);

  int rc = mount(
      source.c_str(), mountPath.c_str(), "nfs", mountFlags, mountOpts.c_str());
  checkUnixError(rc, "failed to mount");
#endif
}

void PrivHelperServer::bindMount(
    const char* clientPath,
    const char* mountPath) {
#ifdef __APPLE__
  (void)clientPath;
  (void)mountPath;
  throw std::runtime_error("this system does not support bind mounts");
#else
  const int rc =
      mount(clientPath, mountPath, /*type*/ nullptr, MS_BIND, /*data*/ nullptr);
  checkUnixError(
      rc, "failed to bind mount `", clientPath, "` over `", mountPath, "`");
#endif
}

void PrivHelperServer::unmount(
    const char* mountPath,
    [[maybe_unused]] UnmountOptions options) {
#ifdef __APPLE__
  auto rc = ::unmount(mountPath, MNT_FORCE);
#else
  // UMOUNT_NOFOLLOW prevents us from following symlinks.
  // This is needed for security, to ensure that we are only unmounting mount
  // points that we originally mounted.  (The processUnmountMsg() call checks
  // to ensure that the path requested matches one that we know about.)
  //
  // MNT_FORCE asks Linux to remove this mount even if it is still "busy"--if
  // there are other processes with open file handles, or in case we failed to
  // unmount some of the bind mounts contained inside it for some reason.
  // This helps ensure that the unmount actually succeeds.
  // This is the same behavior as "umount --force".
  //
  // MNT_DETACH asks Linux to remove the mount from the filesystem immediately.
  // This is the same behavior as "umount --lazy".
  // This is required for the unmount to succeed in some cases, particularly if
  // something has gone wrong and a bind mount still exists inside this mount
  // for some reason.
  //
  // For now we always do forced unmount during shutdown. This helps ensure
  // that edenfs does not get stuck waiting on unmounts to complete.
  // But we also allow non-force unmount via 'edenfsctl rm --no-force' for
  // a more flexible behavior if needed.
  //
  // In the future it might be nice to provide more smarter unmount options.
  int umountFlags = UMOUNT_NOFOLLOW | MNT_DETACH;

  // Only "force" is checked because as this is implemented, we only plan to
  // add "--no-force" as an option. The other options are not checked until
  // we need to support valid use cases for them.
  if (!options.detach || options.expire) {
    XLOG(DFATAL) << "Unsupported unmount option provided: 'detach'"
                 << options.detach;
  }
  if (options.expire) {
    XLOG(DFATAL) << "Unsupported unmount option provided: 'expire'"
                 << options.detach;
  }
  if (options.force) {
    umountFlags |= MNT_FORCE;
  }
  const auto rc = umount2(mountPath, umountFlags);
#endif
  if (rc != 0) {
    const int errnum = errno;
    // EINVAL simply means the path is no longer mounted.
    // This can happen if it was already manually unmounted by a
    // separate process.
    if (errnum != EINVAL) {
      XLOGF(
          WARNING,
          "error unmounting {}: {}",
          mountPath,
          folly::errnoStr(errnum));
    }
  }
}

UnixSocket::Message PrivHelperServer::processTakeoverStartupMsg(
    Cursor& cursor) {
  string mountPath;
  std::vector<string> bindMounts;
  PrivHelperConn::parseTakeoverStartupRequest(cursor, mountPath, bindMounts);
  XLOGF(
      DBG3,
      "takeover startup for \"{}\"; {} bind mounts",
      mountPath,
      bindMounts.size());

  sanityCheckMountPoint(mountPath);

  mountPoints_.insert(mountPath);
  return makeResponse();
}

UnixSocket::Message PrivHelperServer::processMountMsg(Cursor& cursor) {
  string mountPath;
  bool readOnly;
  string vfsType;
  PrivHelperConn::parseMountRequest(cursor, mountPath, readOnly, vfsType);
  XLOGF(DBG3, "mount \"{}\"", mountPath);

  sanityCheckMountPoint(mountPath);

  auto fuseDev = fuseMount(mountPath.c_str(), readOnly, vfsType.c_str());
  mountPoints_.insert(mountPath);

  return makeResponse(std::move(fuseDev));
}

UnixSocket::Message PrivHelperServer::processMountNfsMsg(Cursor& cursor) {
  string mountPath;
  NFSMountOptions options;
  PrivHelperConn::parseMountNfsRequest(cursor, mountPath, options);
  XLOGF(DBG3, "mount.nfs \"{}\"", mountPath);

  sanityCheckMountPoint(mountPath, /*isNFS=*/true, !options.useSoftMount);

  nfsMount(mountPath, std::move(options));
  mountPoints_.insert(mountPath);

  return makeResponse();
}

UnixSocket::Message PrivHelperServer::processUnmountMsg(Cursor& cursor) {
  string mountPath;
  UnmountOptions options;
  PrivHelperConn::parseUnmountRequest(cursor, mountPath, options);
  XLOGF(DBG3, "unmount \"{}\"", mountPath);

  const auto it = mountPoints_.find(mountPath);
  if (it == mountPoints_.end()) {
    throwf<std::domain_error>("No FUSE mount found for {}", mountPath);
  }

  unmount(mountPath.c_str(), options);
  mountPoints_.erase(mountPath);
  return makeResponse();
}

UnixSocket::Message PrivHelperServer::processNfsUnmountMsg(Cursor& cursor) {
  string mountPath;
  PrivHelperConn::parseNfsUnmountRequest(cursor, mountPath);
  XLOGF(DBG3, "unmount \"{}\"", mountPath);

  const auto it = mountPoints_.find(mountPath);
  if (it == mountPoints_.end()) {
    throwf<std::domain_error>("No NFS mount found for {}", mountPath);
  }

  unmount(mountPath.c_str(), {});
  mountPoints_.erase(mountPath);
  return makeResponse();
}

UnixSocket::Message PrivHelperServer::processTakeoverShutdownMsg(
    Cursor& cursor) {
  string mountPath;
  PrivHelperConn::parseTakeoverShutdownRequest(cursor, mountPath);
  XLOGF(DBG3, "takeover shutdown \"{}\"", mountPath);

  const auto it = mountPoints_.find(mountPath);
  if (it == mountPoints_.end()) {
    throwf<std::domain_error>("No mount found for {}", mountPath);
  }

  mountPoints_.erase(mountPath);
  return makeResponse();
}

std::string PrivHelperServer::findMatchingMountPrefix(folly::StringPiece path) {
  for (const auto& mountPoint : mountPoints_) {
    if (boost::starts_with(path, mountPoint + "/")) {
      return mountPoint;
    }
  }
  throwf<std::domain_error>("No FUSE mount found for {}", path);
}

UnixSocket::Message PrivHelperServer::processBindMountMsg(Cursor& cursor) {
  string clientPath;
  string mountPath;
  PrivHelperConn::parseBindMountRequest(cursor, clientPath, mountPath);
  XLOGF(DBG3, "bind mount \"{}\"", mountPath);

  // findMatchingMountPrefix will throw if mountPath doesn't match
  // any known mount.  We perform this check so that we're not a
  // vector for mounting things in arbitrary places.
  auto key = findMatchingMountPrefix(mountPath);

  bindMount(clientPath.c_str(), mountPath.c_str());
  return makeResponse();
}

UnixSocket::Message PrivHelperServer::processBindUnMountMsg(Cursor& cursor) {
  string mountPath;
  PrivHelperConn::parseBindUnMountRequest(cursor, mountPath);
  XLOGF(DBG3, "bind unmount \"{}\"", mountPath);

  // findMatchingMountPrefix will throw if mountPath doesn't match
  // any known mount.  We perform this check so that we're not a
  // vector for arbitrarily unmounting things.
  findMatchingMountPrefix(mountPath);

  bindUnmount(mountPath.c_str());

  return makeResponse();
}

UnixSocket::Message PrivHelperServer::processSetLogFileMsg(
    folly::io::Cursor& cursor,
    UnixSocket::Message& request) {
  XLOG(DBG3, "set log file");
  PrivHelperConn::parseSetLogFileRequest(cursor);
  if (request.files.size() != 1) {
    throwf<std::runtime_error>(
        "expected to receive 1 file descriptor with setLogFile() request. "
        "received {}",
        request.files.size());
  }

  setLogFile(std::move(request.files[0]));

  return makeResponse();
}

void PrivHelperServer::setLogFile(folly::File logFile) {
  // Replace stdout and stderr with the specified file descriptor
  folly::checkUnixError(dup2(logFile.fd(), STDOUT_FILENO));
  folly::checkUnixError(dup2(logFile.fd(), STDERR_FILENO));
}

UnixSocket::Message PrivHelperServer::processSetDaemonTimeout(
    folly::io::Cursor& cursor,
    UnixSocket::Message& /* request */) {
  XLOG(DBG3, "set daemon timeout");
  std::chrono::nanoseconds duration;
  PrivHelperConn::parseSetDaemonTimeoutRequest(cursor, duration);

  setDaemonTimeout(duration);

  return makeResponse();
}

void PrivHelperServer::setDaemonTimeout(std::chrono::nanoseconds duration) {
  fuseTimeout_ = duration;
}

UnixSocket::Message PrivHelperServer::processSetUseEdenFs(
    folly::io::Cursor& cursor,
    UnixSocket::Message& /* request */) {
  XLOG(DBG3, "set use /dev/edenfs");
  PrivHelperConn::parseSetUseEdenFsRequest(cursor, useDevEdenFs_);

  return makeResponse();
}

UnixSocket::Message PrivHelperServer::processGetPid() {
  XLOG(DBG3, "get privhelper pid");

  pid_t pid = getpid();
  auto response = makeResponse();
  response.data.unshare();
  folly::io::Appender cursor{&response.data, 0};
  cursor.writeBE<pid_t>(pid);
  return response;
}

UnixSocket::Message PrivHelperServer::processStartFam(
    folly::io::Cursor& cursor) {
  std::vector<std::string> paths;
  string tmpOutputPath;
  string specifiedOutputPath;
  bool shouldUpload;

  PrivHelperConn::parseStartFamRequest(
      cursor, paths, tmpOutputPath, specifiedOutputPath, shouldUpload);

  // sanity check to make sure we have at least one path
  if (paths.empty()) {
    XLOG(ERR)
        << "Empty list of paths: At least one path should be provided to start FAM";
    throwf<std::runtime_error>("expected at least one path to start FAM");
  }

  for (const auto& path : paths) {
    XLOGF(DBG3, "FAM monitoring path with prefix \"{}\"", path);
  }
  XLOGF(DBG3, "FAM logging events to \"{}\"", tmpOutputPath);
  XLOGF(DBG3, "FAM output file will be moved to \"{}\"", specifiedOutputPath);

  auto opts = SpawnedProcess::Options();
  opts.open(
      STDOUT_FILENO,
      canonicalPath(tmpOutputPath),
      OpenFileHandleOptions::writeFile()); // TODO[lxw]: This can fail if the
                                           // folder doesn't exist
  opts.executablePath(canonicalPath(kFamBinaryPath));
  std::vector<std::string> argv = {
      "SCMFileAccessMonitor",
      "--path-prefix",
      paths[0],
      "--events",
      "NOTIFY_OPEN",
      "NOTIFY_CLOSE",
  };

  famProcess_ = std::make_unique<FileAccessMonitorProcess>(
      SpawnedProcess(argv, std::move(opts)),
      std::move(tmpOutputPath),
      std::move(specifiedOutputPath),
      shouldUpload);

  pid_t pid = famProcess_->proc.pid();

  auto response = makeResponse();
  response.data.unshare();
  folly::io::Appender cursorResp{&response.data, 0};
  cursorResp.writeBE<pid_t>(pid);
  return response;
}

UnixSocket::Message PrivHelperServer::processStopFam() {
  string tmpOutputPath = std::move(famProcess_->tmpOutputPath);
  string specifiedOutputPath = std::move(famProcess_->specifiedOutputPath);
  bool shouldUpload = famProcess_->shouldUpload;

  pid_t pid = famProcess_->proc.pid();
  // SIGTERM should be enough to terminate the process immediately, but we'll
  // also try to kill it if it doesn't exit after a short time, e.g. 500ms.
  auto status =
      famProcess_->proc.terminateOrKill(std::chrono::milliseconds(500));
  if (famProcess_->proc.terminated()) {
    XLOG(DBG3) << "FAM process pid: " << pid << " terminated";
  } else {
    XLOG(ERR) << "Failed to terminate FAM pid: {} " << pid;
    XLOG(ERR) << "FAM process status: " << status.str();

    throwf<std::runtime_error>("Failed to terminate FAM pid: {}", pid);
  }

  famProcess_.reset();

  auto response = makeResponse();
  response.data.unshare();
  folly::io::Appender cursor{&response.data, 0};
  PrivHelperConn::serializeStopFamResponse(
      cursor, tmpOutputPath, specifiedOutputPath, shouldUpload);
  return response;
}

UnixSocket::Message PrivHelperServer::processSetMemoryPriorityForProcess(
    folly::io::Cursor& cursor) {
  XLOG(DBG3, "set memory priority for process");
  pid_t pid;
  int priority;
  PrivHelperConn::parseSetMemoryPriorityForProcessRequest(
      cursor, pid, priority);

  setMemoryPriorityForProcess(pid, priority);

  return makeResponse();
}

void PrivHelperServer::setMemoryPriorityForProcess(pid_t pid, int priority) {
  auto processPriority = ProcessPriority{priority};

  if (processPriority.setPrioritiesForProcess(pid)) {
    throwf<std::runtime_error>(
        "failed to set memory priority for process {}", pid);
  }
}

namespace {
/// Get the file system ID, or an errno value on error
folly::Expected<unsigned long, int> getFSID(const char* path) {
  struct statvfs data;
  if (statvfs(path, &data) != 0) {
    return folly::makeUnexpected(errno);
  }
  return folly::makeExpected<int>(data.f_fsid);
}
} // namespace

void PrivHelperServer::bindUnmount(const char* mountPath) {
  // Check the current filesystem information for this path,
  // so we can confirm that it has been unmounted afterwards.
  const auto origFSID = getFSID(mountPath);

  unmount(mountPath, {});

  // Empirically, the unmount may not be complete when umount2() returns.
  // To work around this, we repeatedly invoke statvfs() on the bind mount
  // until it fails or returns a different filesystem ID.
  //
  // Give up after 2 seconds even if the unmount does not appear complete.
  constexpr auto timeout = std::chrono::seconds(2);
  const auto endTime = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto fsid = getFSID(mountPath);
    if (!fsid.hasValue()) {
      // Assume the file system is unmounted if the statvfs() call failed.
      break;
    }
    if (origFSID.hasValue() && origFSID.value() != fsid.value()) {
      // The unmount has succeeded if the filesystem ID is different now.
      break;
    }

    if (std::chrono::steady_clock::now() > endTime) {
      XLOGF(
          WARNING,
          "error unmounting {}: mount did not go away after successful unmount call",
          mountPath);
      break;
    }
    sched_yield();
  }
}

void PrivHelperServer::run() {
  // Ignore SIGINT and SIGTERM.
  // We should only exit when our parent process does.
  // (Normally if someone hits Ctrl-C in their terminal this will send SIGINT
  // to both our parent process and to us.  The parent process should exit due
  // to this signal.  We don't want to exit immediately--we want to wait until
  // the parent exits and then umount all outstanding mount points before we
  // exit.)
  if (signal(SIGINT, SIG_IGN) == SIG_ERR) {
    XLOGF(
        FATAL,
        "error setting SIGINT handler in privhelper process: {}",
        folly::errnoStr(errno));
  }
  if (signal(SIGTERM, SIG_IGN) == SIG_ERR) {
    XLOGF(
        FATAL,
        "error setting SIGTERM handler in privhelper process: {}",
        folly::errnoStr(errno));
  }

  conn_->setReceiveCallback(this);
  eventBase_->loop();

  // We terminate the event loop when the socket has been closed.
  // This normally means the parent process exited, so we can clean up and exit
  // too.
  XLOG(DBG5, "privhelper process exiting");

  // Unmount all active mount points
  cleanupMountPoints();
}

void PrivHelperServer::messageReceived(UnixSocket::Message&& message) noexcept {
  try {
    processAndSendResponse(std::move(message));
  } catch (const std::exception& ex) {
    XLOGF(
        ERR,
        "error processing privhelper request: {}",
        folly::exceptionStr(ex));
  }
}

void PrivHelperServer::processAndSendResponse(UnixSocket::Message&& message) {
  Cursor cursor{&message.data};
  PrivHelperConn::PrivHelperPacket packet = PrivHelperConn::parsePacket(cursor);

  UnixSocket::Message response;
  try {
    response = processMessage(packet, cursor, message);
  } catch (const std::exception& ex) {
    XLOGF(
        ERR,
        "error processing privhelper request: {}",
        folly::exceptionStr(ex));
    packet.metadata.msg_type = PrivHelperConn::RESP_ERROR;
    response = makeResponse();
    Appender appender(&response.data, 1024);
    PrivHelperConn::serializeErrorResponse(appender, ex);
  }

  // Put the version, transaction ID, and message type in the response.
  // The makeResponse() APIs ensure there is enough headroom for the packet.
  // Version info is important, since we may fall back to an older version
  // in order to process a request.
  size_t packetSize = sizeof(packet);
  if (response.data.headroom() >= packetSize) {
    response.data.prepend(packetSize);
  } else {
    // This is unexpected, but go ahead and allocate more room just in case this
    // ever does occur.
    XLOG(
        WARN,
        "insufficient headroom for privhelper response packet; making more space");
    auto body = std::make_unique<IOBuf>(std::move(response.data));
    response.data = IOBuf(IOBuf::CREATE, packetSize);
    response.data.append(packetSize);
    response.data.prependChain(std::move(body));
  }

  RWPrivateCursor respCursor(&response.data);

  PrivHelperConn::serializeResponsePacket(packet, respCursor);

  conn_->send(std::move(response));
}

UnixSocket::Message PrivHelperServer::makeResponse() {
  // 1024 bytes is enough for most responses.  If the response is longer
  // we will allocate more room later.
  constexpr size_t kDefaultBufferSize = 1024;

  UnixSocket::Message msg;
  msg.data = IOBuf(IOBuf::CREATE, kDefaultBufferSize);

  // Leave enough headroom for the response packet that includes the transaction
  // ID and message type (and any additional metadata).
  msg.data.advance(sizeof(PrivHelperConn::PrivHelperPacket));
  return msg;
}

UnixSocket::Message PrivHelperServer::makeResponse(folly::File file) {
  auto response = makeResponse();
  response.files.push_back(std::move(file));
  return response;
}

UnixSocket::Message PrivHelperServer::processMessage(
    PrivHelperConn::PrivHelperPacket& packet,
    Cursor& cursor,
    UnixSocket::Message& request) {
  // TODO(T185426586): In the future, we can use packet.header.version to
  // decide how to handle each request. Each request handler can implement
  // different handler logic for each known version (if needed).
  PrivHelperConn::MsgType msgType{packet.metadata.msg_type};
  XLOGF(
      DBG7,
      "Processing message of type {} for protocol version v{}",
      msgType,
      packet.header.version);
  switch (msgType) {
    case PrivHelperConn::REQ_MOUNT_FUSE:
      return processMountMsg(cursor);
    case PrivHelperConn::REQ_MOUNT_NFS:
      return processMountNfsMsg(cursor);
    case PrivHelperConn::REQ_MOUNT_BIND:
      return processBindMountMsg(cursor);
    case PrivHelperConn::REQ_UNMOUNT_FUSE:
      return processUnmountMsg(cursor);
    case PrivHelperConn::REQ_UNMOUNT_NFS:
      return processNfsUnmountMsg(cursor);
    case PrivHelperConn::REQ_TAKEOVER_SHUTDOWN:
      return processTakeoverShutdownMsg(cursor);
    case PrivHelperConn::REQ_TAKEOVER_STARTUP:
      return processTakeoverStartupMsg(cursor);
    case PrivHelperConn::REQ_SET_LOG_FILE:
      return processSetLogFileMsg(cursor, request);
    case PrivHelperConn::REQ_UNMOUNT_BIND:
      return processBindUnMountMsg(cursor);
    case PrivHelperConn::REQ_SET_DAEMON_TIMEOUT:
      return processSetDaemonTimeout(cursor, request);
    case PrivHelperConn::REQ_SET_USE_EDENFS:
      return processSetUseEdenFs(cursor, request);
    case PrivHelperConn::REQ_GET_PID:
      return processGetPid();
    case PrivHelperConn::REQ_START_FAM:
      return processStartFam(cursor);
    case PrivHelperConn::REQ_STOP_FAM:
      return processStopFam();
    case PrivHelperConn::REQ_SET_MEMORY_PRIORITY_FOR_PROCESS:
      return processSetMemoryPriorityForProcess(cursor);
    case PrivHelperConn::MSG_TYPE_NONE:
    case PrivHelperConn::RESP_ERROR:
      break;
  }

  throwf<std::runtime_error>(
      "unexpected privhelper message type: {}", folly::to_underlying(msgType));
}

void PrivHelperServer::eofReceived() noexcept {
  eventBase_->terminateLoopSoon();
}

void PrivHelperServer::socketClosed() noexcept {
  eventBase_->terminateLoopSoon();
}

void PrivHelperServer::receiveError(
    const folly::exception_wrapper& ew) noexcept {
  XLOGF(ERR, "receive error in privhelper server: {}", ew.what());
  eventBase_->terminateLoopSoon();
}

void PrivHelperServer::cleanupMountPoints() {
  for (const auto& mountPoint : mountPoints_) {
    try {
      unmount(mountPoint.c_str(), {});
    } catch (const std::exception& ex) {
      XLOGF(
          ERR,
          "error unmounting \"{}\": {}",
          mountPoint,
          folly::exceptionStr(ex));
    }
  }

  mountPoints_.clear();
}

} // namespace facebook::eden

#endif
